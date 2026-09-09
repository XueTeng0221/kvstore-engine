#include "kvstore/persistence/aof.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <sstream>

#include "kvstore/common/crc32.hpp"

namespace kvstore {

AofWriter::AofWriter(std::filesystem::path directory, std::size_t record_limit,
                     std::size_t byte_limit, std::uint64_t interval_ms, std::string sync_policy)
    : path_(std::move(directory) / "appendonly.aof"),
      record_limit_(record_limit),
      byte_limit_(byte_limit),
      interval_ms_(interval_ms),
      sync_policy_(std::move(sync_policy)),
      last_flush_(std::chrono::steady_clock::now()) {
  std::filesystem::create_directories(path_.parent_path());
  output_.Reset(::open(path_.c_str(), O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC, 0644));
}

AofWriter::~AofWriter() { static_cast<void>(Flush(true)); }

Status AofWriter::Append(const WriteEvent& event) {
  bool should_flush = false;
  {
    std::scoped_lock lock(mutex_);
    if (!output_) return {StatusCode::kIoError, "AOF is not open"};
    const std::string payload = event.key + event.value;
    const std::string metadata =
        "AOF1 " + std::to_string(event.offset) + " " + std::to_string(event.event_id) + " " +
        std::to_string(static_cast<int>(event.type)) + " " +
        std::to_string(static_cast<int>(event.source)) + " " + std::to_string(event.key.size()) +
        " " + std::to_string(event.value.size());
    const std::string checksummed = metadata + "\n" + payload;
    const std::string record =
        metadata + " " +
        std::to_string(Crc32(std::as_bytes(std::span(checksummed.data(), checksummed.size())))) +
        "\n" + payload;
    std::size_t written = 0;
    while (written < record.size()) {
      const ssize_t count =
          ::write(output_.get(), record.data() + written, record.size() - written);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return {StatusCode::kIoError, "AOF write failed"};
      written += static_cast<std::size_t>(count);
    }
    ++pending_records_;
    pending_bytes_ += record.size();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - last_flush_);
    should_flush = sync_policy_ == "always" || sync_policy_ == "everysec" ||
                   pending_records_ >= record_limit_ || pending_bytes_ >= byte_limit_ ||
                   static_cast<std::uint64_t>(elapsed.count()) >= interval_ms_;
  }
  return should_flush ? Flush(false) : Status::Ok();
}

Status AofWriter::Flush(bool force_sync) {
  std::scoped_lock lock(mutex_);
  if (!output_) return {StatusCode::kIoError, "AOF is not open"};
  if (force_sync || sync_policy_ == "always" || sync_policy_ == "everysec") {
    if (::fdatasync(output_.get()) < 0) return {StatusCode::kIoError, "AOF sync failed"};
  }
  pending_records_ = 0;
  pending_bytes_ = 0;
  last_flush_ = std::chrono::steady_clock::now();
  return Status::Ok();
}

Result<std::uint64_t> AofWriter::Replay(IEngine& engine, std::uint64_t minimum_offset) {
  std::ifstream input(path_, std::ios::binary);
  if (!input) return std::uint64_t{0};
  std::uint64_t previous_offset = 0;
  std::uint64_t previous_event_id = 0;
  while (input.peek() != std::char_traits<char>::eof()) {
    std::string header;
    if (!std::getline(input, header))
      return Status{StatusCode::kCorruption, "truncated AOF header"};
    std::istringstream fields(header);
    std::string header_magic;
    std::uint64_t offset = 0;
    std::uint64_t event_id = 0;
    int type_number = 0;
    int source_number = 0;
    std::size_t key_size = 0;
    std::size_t value_size = 0;
    std::uint32_t expected_crc = 0;
    if (!(fields >> header_magic >> offset >> event_id >> type_number >> source_number >>
          key_size >> value_size >> expected_crc) ||
        header_magic != "AOF1" || type_number < 0 ||
        type_number > static_cast<int>(CommandType::kInfo) || source_number < 0 ||
        source_number > static_cast<int>(WriteSource::kIncrementalSync) ||
        key_size > std::numeric_limits<std::size_t>::max() - value_size) {
      return Status{StatusCode::kCorruption, "invalid AOF header"};
    }
    if (offset == 0 || event_id == 0 || offset <= previous_offset ||
        event_id <= previous_event_id || (previous_offset != 0 && offset != previous_offset + 1)) {
      return Status{StatusCode::kCorruption, "non-monotonic AOF event"};
    }
    previous_offset = offset;
    previous_event_id = event_id;
    std::string payload(key_size + value_size, '\0');
    input.read(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (input.gcount() != static_cast<std::streamsize>(payload.size())) {
      return Status{StatusCode::kCorruption, "truncated AOF payload"};
    }
    const std::string metadata = "AOF1 " + std::to_string(offset) + " " + std::to_string(event_id) +
                                 " " + std::to_string(type_number) + " " +
                                 std::to_string(source_number) + " " + std::to_string(key_size) +
                                 " " + std::to_string(value_size);
    const std::string checksummed = metadata + "\n" + payload;
    if (Crc32(std::as_bytes(std::span(checksummed.data(), checksummed.size()))) != expected_crc) {
      return Status{StatusCode::kCorruption, "AOF checksum"};
    }
    if (offset <= minimum_offset) continue;
    if (type_number != static_cast<int>(CommandType::kSet) &&
        type_number != static_cast<int>(CommandType::kMod) &&
        type_number != static_cast<int>(CommandType::kDel) &&
        type_number != static_cast<int>(CommandType::kIncr) &&
        type_number != static_cast<int>(CommandType::kDecr))
      return Status{StatusCode::kCorruption, "non-mutation AOF command"};
    Command command{static_cast<CommandType>(type_number),
                    {payload.substr(0, key_size), payload.substr(key_size, value_size)},
                    WriteSource::kAofReplay};
    Status status = Status::Ok();
    if (command.type == CommandType::kSet) {
      auto result = engine.Upsert(command.args[0], command.args[1]);
      if (!result.ok()) status = result.status();
    } else if (command.type == CommandType::kMod)
      status = engine.Modify(command.args[0], command.args[1]);
    else if (command.type == CommandType::kDel)
      status = engine.Delete(command.args[0]);
    else if (command.type == CommandType::kIncr || command.type == CommandType::kDecr) {
      std::int64_t delta = 0;
      const auto [end, error] = std::from_chars(
          command.args[1].data(), command.args[1].data() + command.args[1].size(), delta);
      if (error != std::errc{} || end != command.args[1].data() + command.args[1].size())
        return Status{StatusCode::kCorruption, "invalid AOF increment"};
      auto result = engine.Increment(command.args[0], delta);
      if (!result.ok()) status = result.status();
    }
    if (!status.ok() && status.code() != StatusCode::kAlreadyExists &&
        status.code() != StatusCode::kNotFound)
      return status;
  }
  return previous_offset;
}

}  // namespace kvstore
