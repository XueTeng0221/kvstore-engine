#include "kvstore/persistence/aof.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <span>
#include <thread>

#include "kvstore/common/crc32.hpp"

namespace kvstore {
namespace {

constexpr std::size_t kHeaderBytes = 44;
constexpr std::size_t kEventHeaderBytes = 36;
constexpr std::size_t kTrailerBytes = 8;
constexpr std::size_t kMaxTransactionBytes = 128U * 1024U * 1024U;
constexpr char kMagic[] = "AOT1";
constexpr char kCommit[] = "AOC1";

template <typename T>
void Put(std::string& output, T value) {
  for (std::size_t index = 0; index < sizeof(T); ++index)
    output.push_back(static_cast<char>(value >> ((sizeof(T) - index - 1U) * 8U)));
}

template <typename T>
bool Get(std::string_view input, std::size_t& position, T& value) {
  if (position > input.size() || sizeof(T) > input.size() - position) return false;
  value = 0;
  for (std::size_t index = 0; index < sizeof(T); ++index)
    value = static_cast<T>((value << 8U) | static_cast<unsigned char>(input[position++]));
  return true;
}

bool Mutation(CommandType type) {
  return type == CommandType::kSet || type == CommandType::kMod || type == CommandType::kDel ||
         type == CommandType::kDelMany || type == CommandType::kIncr ||
         type == CommandType::kDecr || type == CommandType::kIncrBy || type == CommandType::kDecrBy;
}

Result<std::vector<std::string>> DecodeKeys(std::string_view value) {
  std::vector<std::string> keys;
  std::size_t position = 0;
  while (position < value.size()) {
    if (value.size() - position < sizeof(std::uint32_t))
      return Status{StatusCode::kCorruption, "invalid multi-delete event"};
    std::uint32_t size = 0;
    for (std::size_t index = 0; index < sizeof(size); ++index)
      size =
          static_cast<std::uint32_t>((size << 8U) | static_cast<unsigned char>(value[position++]));
    if (size > value.size() - position)
      return Status{StatusCode::kCorruption, "invalid multi-delete key"};
    keys.emplace_back(value.substr(position, size));
    position += size;
  }
  return keys;
}

Status WriteAll(int fd, std::string_view bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    const auto count = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return {StatusCode::kIoError, "AOF write failed"};
    written += static_cast<std::size_t>(count);
  }
  return Status::Ok();
}

}  // namespace

AofWriter::AofWriter(std::filesystem::path directory, std::size_t record_limit,
                     std::size_t byte_limit, std::uint64_t interval_ms, std::string sync_policy,
                     std::uint64_t max_file_bytes)
    : path_(std::move(directory) / "appendonly.aof"),
      record_limit_(record_limit),
      byte_limit_(byte_limit),
      interval_ms_(interval_ms),
      sync_policy_(std::move(sync_policy)),
      max_file_bytes_(max_file_bytes),
      last_flush_(std::chrono::steady_clock::now()) {
  std::filesystem::create_directories(path_.parent_path());
  const bool existed = std::filesystem::exists(path_);
  output_.Reset(::open(path_.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644));
  if (output_ && !existed) {
    const UniqueFd directory_fd(
        ::open(path_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!directory_fd || ::fsync(directory_fd.get()) < 0) failed_ = true;
  }
  if (sync_policy_ == "everysec") {
    sync_thread_ = OwnedThread([this](std::stop_token stop) {
      const auto interval = std::chrono::milliseconds(interval_ms_);
      const auto poll = std::min(interval, std::chrono::milliseconds(50));
      auto elapsed = std::chrono::milliseconds(0);
      while (!stop.stop_requested()) {
        std::this_thread::sleep_for(poll);
        if (stop.stop_requested()) break;
        elapsed += poll;
        if (elapsed >= interval) {
          static_cast<void>(Flush(true));
          elapsed = std::chrono::milliseconds(0);
        }
      }
    });
  }
}

AofWriter::~AofWriter() {
  if (sync_thread_.joinable()) {
    sync_thread_.request_stop();
    sync_thread_.join();
  }
  static_cast<void>(Flush(false));
}

Status AofWriter::Append(const WriteEvent& event) {
  return AppendBatch(std::vector<WriteEvent>{event});
}

Status AofWriter::AppendBatch(const std::vector<WriteEvent>& events) {
  if (events.empty()) return Status::Ok();
  std::scoped_lock lock(mutex_);
  if (!output_ || failed_) return {StatusCode::kIoError, "AOF is unavailable"};
  if (events.size() > std::numeric_limits<std::uint32_t>::max())
    return {StatusCode::kLimitExceeded, "AOF transaction event limit"};

  std::string payload;
  std::uint64_t expected_offset = last_offset_;
  std::uint64_t expected_event_id = last_event_id_;
  for (const auto& event : events) {
    if (!Mutation(event.type) || event.offset == 0 || event.event_id == 0 ||
        (expected_offset != 0 && event.offset != expected_offset + 1) ||
        (expected_event_id != 0 && event.event_id != expected_event_id + 1) ||
        event.key.size() > std::numeric_limits<std::uint64_t>::max() - event.value.size() ||
        event.origin_node.size() > std::numeric_limits<std::uint32_t>::max())
      return {StatusCode::kInvalidArgument, "invalid AOF event"};
    if (event.checksum != event.ComputeChecksum())
      return {StatusCode::kInvalidArgument, "invalid write event checksum"};
    const auto event_bytes =
        kEventHeaderBytes + event.key.size() + event.value.size() + event.origin_node.size();
    if (event_bytes > kMaxTransactionBytes || payload.size() > kMaxTransactionBytes - event_bytes)
      return {StatusCode::kLimitExceeded, "AOF transaction size limit"};
    Put<std::uint8_t>(payload, static_cast<std::uint8_t>(event.source));
    Put<std::uint8_t>(payload, static_cast<std::uint8_t>(event.type));
    Put<std::uint16_t>(payload, 0);
    Put<std::uint64_t>(payload, event.key.size());
    Put<std::uint64_t>(payload, event.value.size());
    Put<std::uint32_t>(payload, static_cast<std::uint32_t>(event.origin_node.size()));
    Put<std::uint64_t>(payload, event.timestamp_ms);
    Put<std::uint32_t>(payload, event.checksum);
    payload.append(event.key);
    payload.append(event.value);
    payload.append(event.origin_node);
    expected_offset = event.offset;
    expected_event_id = event.event_id;
  }

  std::string transaction(kMagic, sizeof(kMagic) - 1U);
  Put<std::uint16_t>(transaction, 2);
  Put<std::uint16_t>(transaction, 0);
  Put<std::uint32_t>(transaction, static_cast<std::uint32_t>(events.size()));
  Put<std::uint64_t>(transaction, payload.size());
  Put<std::uint64_t>(transaction, events.front().offset);
  Put<std::uint64_t>(transaction, events.front().event_id);
  Put<std::uint32_t>(transaction, 0);
  Put<std::uint32_t>(transaction, 0);
  const auto header_crc = Crc32(
      std::as_bytes(std::span(transaction.data(), transaction.size() - sizeof(std::uint32_t))));
  for (std::size_t index = 0; index < sizeof(header_crc); ++index)
    transaction[40 + index] =
        static_cast<char>(header_crc >> ((sizeof(header_crc) - index - 1U) * 8U));
  transaction += payload;
  const auto crc = Crc32(std::as_bytes(std::span(transaction.data(), transaction.size())));
  for (std::size_t index = 0; index < sizeof(crc); ++index)
    transaction[36 + index] = static_cast<char>(crc >> ((sizeof(crc) - index - 1U) * 8U));
  transaction.append(kCommit, sizeof(kCommit) - 1U);
  Put<std::uint32_t>(transaction, crc);

  const auto start = ::lseek(output_.get(), 0, SEEK_END);
  if (start < 0) return {StatusCode::kIoError, "AOF seek failed"};
  const auto current_size = static_cast<std::uint64_t>(start);
  if (current_size > max_file_bytes_ || transaction.size() > max_file_bytes_ - current_size)
    return {StatusCode::kLimitExceeded, "AOF file capacity reached"};
  const auto status = WriteAll(output_.get(), transaction);
  if (!status.ok()) {
    failed_ = true;
    return status;
  }
  if (sync_policy_ == "always" && ::fdatasync(output_.get()) < 0) {
    failed_ = true;
    last_offset_ = events.back().offset;
    last_event_id_ = events.back().event_id;
    return {StatusCode::kInternal, "AOF commit is durable but sync confirmation failed"};
  }
  pending_records_ += events.size();
  pending_bytes_ += transaction.size();
  last_offset_ = events.back().offset;
  last_event_id_ = events.back().event_id;
  const bool threshold_reached = pending_records_ >= record_limit_ || pending_bytes_ >= byte_limit_;
  if (threshold_reached && sync_policy_ == "everysec" && ::fdatasync(output_.get()) < 0) {
    failed_ = true;
    return {StatusCode::kInternal, "AOF threshold sync failed with uncertain commit"};
  }
  if (threshold_reached) {
    pending_records_ = 0;
    pending_bytes_ = 0;
  }
  return Status::Ok();
}

Status AofWriter::Flush(bool force_sync) {
  std::scoped_lock lock(mutex_);
  if (!output_ || failed_) return {StatusCode::kIoError, "AOF is unavailable"};
  if ((force_sync || sync_policy_ == "always" || sync_policy_ == "everysec") &&
      ::fdatasync(output_.get()) < 0) {
    failed_ = true;
    return {StatusCode::kIoError, "AOF sync failed"};
  }
  pending_records_ = 0;
  pending_bytes_ = 0;
  last_flush_ = std::chrono::steady_clock::now();
  return Status::Ok();
}

Result<std::uint64_t> AofWriter::Replay(IEngine& engine, std::uint64_t minimum_offset,
                                        std::uint64_t minimum_event_id) {
  std::scoped_lock lock(mutex_);
  std::ifstream input(path_, std::ios::binary);
  if (!input) return std::uint64_t{0};
  if (std::filesystem::file_size(path_) > max_file_bytes_)
    return Status{StatusCode::kLimitExceeded, "AOF file exceeds configured capacity"};
  auto exported = engine.Export();
  if (!exported.ok()) return exported.status();
  std::map<std::string, std::string> state;
  for (auto& entry : exported.value()) state.emplace(std::move(entry.key), std::move(entry.value));

  std::uint64_t previous_offset = 0;
  std::uint64_t previous_event_id = 0;
  bool replay_boundary_seen = minimum_offset == 0;
  std::streamoff valid_end = 0;
  bool incomplete_tail = false;
  while (input.peek() != std::char_traits<char>::eof()) {
    std::string header(kHeaderBytes, '\0');
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    if (input.gcount() != static_cast<std::streamsize>(header.size())) {
      const auto partial = static_cast<std::size_t>(input.gcount());
      const auto prefix = std::min<std::size_t>(partial, 4);
      if (input.eof() && partial > 0 && std::memcmp(header.data(), kMagic, prefix) == 0) {
        incomplete_tail = true;
        break;
      }
      return Status{StatusCode::kCorruption, "truncated AOF transaction header"};
    }
    if (std::memcmp(header.data(), kMagic, 4) != 0)
      return Status{StatusCode::kCorruption, "AOF transaction magic"};
    std::size_t position = 4;
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    std::uint32_t count = 0;
    std::uint64_t payload_size = 0;
    std::uint64_t first_offset = 0;
    std::uint64_t first_event_id = 0;
    std::uint32_t expected_crc = 0;
    std::uint32_t expected_header_crc = 0;
    if (!Get(header, position, version) || !Get(header, position, flags) ||
        !Get(header, position, count) || !Get(header, position, payload_size) ||
        !Get(header, position, first_offset) || !Get(header, position, first_event_id) ||
        !Get(header, position, expected_crc) || !Get(header, position, expected_header_crc) ||
        version != 2 || flags != 0 || count == 0 || payload_size > kMaxTransactionBytes)
      return Status{StatusCode::kCorruption, "invalid AOF transaction header"};
    std::string header_crc_input = header.substr(0, 40);
    std::fill(header_crc_input.begin() + 36, header_crc_input.begin() + 40, 0);
    if (Crc32(std::as_bytes(std::span(header_crc_input.data(), header_crc_input.size()))) !=
        expected_header_crc)
      return Status{StatusCode::kCorruption, "AOF transaction header checksum"};
    std::string payload(static_cast<std::size_t>(payload_size), '\0');
    input.read(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (input.gcount() != static_cast<std::streamsize>(payload.size())) {
      incomplete_tail = true;
      break;
    }
    std::string trailer(kTrailerBytes, '\0');
    input.read(trailer.data(), static_cast<std::streamsize>(trailer.size()));
    if (input.gcount() != static_cast<std::streamsize>(trailer.size())) {
      incomplete_tail = true;
      break;
    }
    std::size_t trailer_position = 4;
    std::uint32_t trailer_crc = 0;
    if (trailer.substr(0, 4) != kCommit || !Get(trailer, trailer_position, trailer_crc) ||
        trailer_crc != expected_crc)
      return Status{StatusCode::kCorruption, "invalid AOF transaction commit"};
    std::string crc_input = header;
    std::fill(crc_input.begin() + 36, crc_input.begin() + 40, 0);
    crc_input += payload;
    if (Crc32(std::as_bytes(std::span(crc_input.data(), crc_input.size()))) != expected_crc)
      return Status{StatusCode::kCorruption, "AOF transaction checksum"};

    position = 0;
    std::uint64_t offset = first_offset;
    std::uint64_t event_id = first_event_id;
    for (std::uint32_t index = 0; index < count; ++index, ++offset, ++event_id) {
      std::uint8_t source_number = 0;
      std::uint8_t type_number = 0;
      std::uint16_t reserved = 0;
      std::uint64_t key_size = 0;
      std::uint64_t value_size = 0;
      std::uint32_t origin_size = 0;
      std::uint64_t timestamp_ms = 0;
      std::uint32_t event_checksum = 0;
      if (!Get(payload, position, source_number) || !Get(payload, position, type_number) ||
          !Get(payload, position, reserved) || !Get(payload, position, key_size) ||
          !Get(payload, position, value_size) || !Get(payload, position, origin_size) ||
          !Get(payload, position, timestamp_ms) || !Get(payload, position, event_checksum) ||
          reserved != 0 ||
          source_number > static_cast<std::uint8_t>(WriteSource::kIncrementalSync) ||
          type_number > static_cast<std::uint8_t>(CommandType::kInfo) ||
          key_size > payload.size() - position ||
          value_size > payload.size() - position - key_size ||
          origin_size > payload.size() - position - key_size - value_size)
        return Status{StatusCode::kCorruption, "invalid AOF event"};
      const auto key = payload.substr(position, static_cast<std::size_t>(key_size));
      position += static_cast<std::size_t>(key_size);
      const auto value = payload.substr(position, static_cast<std::size_t>(value_size));
      position += static_cast<std::size_t>(value_size);
      const auto origin = payload.substr(position, origin_size);
      position += origin_size;
      const WriteEvent event{offset,
                             event_id,
                             static_cast<WriteSource>(source_number),
                             static_cast<CommandType>(type_number),
                             key,
                             value,
                             origin,
                             timestamp_ms,
                             event_checksum};
      if (event.checksum != event.ComputeChecksum())
        return Status{StatusCode::kCorruption, "write event checksum"};
      if (offset <= previous_offset || event_id <= previous_event_id ||
          (previous_offset != 0 && offset != previous_offset + 1) ||
          (previous_event_id != 0 && event_id != previous_event_id + 1))
        return Status{StatusCode::kCorruption, "non-contiguous AOF event"};
      previous_offset = offset;
      previous_event_id = event_id;
      if (offset <= minimum_offset) {
        if (offset == minimum_offset && event_id != minimum_event_id)
          return Status{StatusCode::kCorruption, "AOF snapshot event id mismatch"};
        continue;
      }
      if (!replay_boundary_seen &&
          (offset != minimum_offset + 1 || event_id != minimum_event_id + 1))
        return Status{StatusCode::kCorruption, "AOF does not continue snapshot"};
      replay_boundary_seen = true;
      const auto type = static_cast<CommandType>(type_number);
      if (type == CommandType::kSet || type == CommandType::kMod)
        state[key] = value;
      else if (type == CommandType::kDel)
        state.erase(key);
      else if (type == CommandType::kDelMany) {
        auto keys = DecodeKeys(value);
        if (!keys.ok()) return keys.status();
        for (const auto& deleted_key : keys.value()) state.erase(deleted_key);
      } else if (type == CommandType::kIncr || type == CommandType::kDecr ||
                 type == CommandType::kIncrBy || type == CommandType::kDecrBy) {
        std::int64_t delta = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), delta);
        if (error != std::errc{} || end != value.data() + value.size())
          return Status{StatusCode::kCorruption, "invalid AOF increment"};
        std::int64_t current = 0;
        if (const auto iterator = state.find(key); iterator != state.end()) {
          const auto [current_end, current_error] = std::from_chars(
              iterator->second.data(), iterator->second.data() + iterator->second.size(), current);
          if (current_error != std::errc{} ||
              current_end != iterator->second.data() + iterator->second.size())
            return Status{StatusCode::kCorruption, "invalid AOF integer state"};
        }
        if ((delta > 0 && current > std::numeric_limits<std::int64_t>::max() - delta) ||
            (delta < 0 && current < std::numeric_limits<std::int64_t>::min() - delta))
          return Status{StatusCode::kCorruption, "AOF increment overflow"};
        state[key] = std::to_string(current + delta);
      } else {
        return Status{StatusCode::kCorruption, "non-mutation AOF command"};
      }
    }
    if (position != payload.size()) return Status{StatusCode::kCorruption, "AOF trailing payload"};
    valid_end = input.tellg();
  }
  if (previous_offset != 0 && previous_offset < minimum_offset)
    return Status{StatusCode::kCorruption, "AOF ends before snapshot"};
  std::vector<Entry> entries;
  entries.reserve(state.size());
  for (auto& [key, value] : state) entries.push_back({std::move(key), std::move(value)});
  const auto imported = engine.Import(entries);
  if (!imported.ok()) return imported;
  if (incomplete_tail) {
    if (::ftruncate(output_.get(), static_cast<off_t>(valid_end)) < 0 ||
        ::fdatasync(output_.get()) < 0) {
      failed_ = true;
      return Status{StatusCode::kIoError, "cannot remove incomplete AOF tail"};
    }
  }
  last_offset_ = previous_offset;
  last_event_id_ = previous_event_id;
  return previous_offset;
}

}  // namespace kvstore
