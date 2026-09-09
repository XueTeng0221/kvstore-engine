#include "kvstore/persistence/snapshot.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <vector>

#include "kvstore/common/crc32.hpp"
#include "kvstore/common/resources.hpp"

namespace kvstore {
namespace {

constexpr std::array<char, 8> kMagic{'K', 'V', 'S', 'N', 'A', 'P', '1', '\0'};

template <typename T>
void Put(std::string& output, T value) {
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    output.push_back(static_cast<char>(value >> ((sizeof(T) - index - 1) * 8U)));
  }
}

template <typename T>
bool Get(std::string_view input, std::size_t& position, T& value) {
  if (position > input.size() || sizeof(T) > input.size() - position) return false;
  value = 0;
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    value = static_cast<T>((value << 8U) | static_cast<unsigned char>(input[position++]));
  }
  return true;
}

}  // namespace

Status Snapshot::Save(const std::filesystem::path& directory, const IEngine& engine,
                      std::uint64_t last_offset) {
  const auto entries = engine.Export();
  if (!entries.ok()) return entries.status();
  std::string payload;
  Put<std::uint64_t>(payload, entries.value().size());
  for (const auto& entry : entries.value()) {
    Put<std::uint64_t>(payload, entry.key.size());
    Put<std::uint64_t>(payload, entry.value.size());
    payload.append(entry.key);
    payload.append(entry.value);
  }
  std::string header(kMagic.data(), kMagic.size());
  Put<std::uint32_t>(header, kVersion);
  Put<std::uint64_t>(header, last_offset);
  Put<std::uint64_t>(header, payload.size());
  std::string crc_input = header + payload;
  const auto crc = Crc32(std::as_bytes(std::span(crc_input.data(), crc_input.size())));
  Put<std::uint32_t>(header, crc);
  std::filesystem::create_directories(directory);
  const auto temporary = directory / "snapshot.tmp";
  const auto final = directory / "snapshot.kvs";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) return {StatusCode::kIoError, "cannot create snapshot"};
  output.write(header.data(), static_cast<std::streamsize>(header.size()));
  output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  output.flush();
  if (!output) return {StatusCode::kIoError, "cannot write snapshot"};
  output.close();
  const UniqueFd file_fd(::open(temporary.c_str(), O_RDONLY | O_CLOEXEC));
  if (!file_fd || ::fsync(file_fd.get()) < 0)
    return {StatusCode::kIoError, "snapshot fsync failed"};
  std::filesystem::rename(temporary, final);
  const UniqueFd directory_fd(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!directory_fd || ::fsync(directory_fd.get()) < 0)
    return {StatusCode::kIoError, "snapshot directory fsync failed"};
  return Status::Ok();
}

Result<std::uint64_t> Snapshot::Load(const std::filesystem::path& directory, IEngine& engine) {
  const auto path = directory / "snapshot.kvs";
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::uint64_t{0};
  const auto size = std::filesystem::file_size(path);
  if (size < 32 || size > std::numeric_limits<std::size_t>::max()) {
    return Status{StatusCode::kCorruption, "invalid snapshot size"};
  }
  std::string bytes(static_cast<std::size_t>(size), '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!input) return Status{StatusCode::kCorruption, "truncated snapshot"};
  if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
    return Status{StatusCode::kCorruption, "snapshot magic"};
  std::size_t position = kMagic.size();
  std::uint32_t version = 0;
  std::uint64_t offset = 0;
  std::uint64_t payload_size = 0;
  std::uint32_t expected_crc = 0;
  if (!Get(bytes, position, version) || !Get(bytes, position, offset) ||
      !Get(bytes, position, payload_size) || !Get(bytes, position, expected_crc) ||
      version != kVersion || payload_size != bytes.size() - position) {
    return Status{StatusCode::kCorruption, "snapshot header"};
  }
  std::string crc_input = bytes.substr(0, position - sizeof(std::uint32_t));
  crc_input.append(bytes.data() + position, payload_size);
  const auto actual_crc = Crc32(std::as_bytes(std::span(crc_input.data(), crc_input.size())));
  if (actual_crc != expected_crc) return Status{StatusCode::kCorruption, "snapshot checksum"};
  std::uint64_t count = 0;
  if (!Get(bytes, position, count)) return Status{StatusCode::kCorruption, "snapshot count"};
  std::vector<Entry> entries;
  if (count > std::numeric_limits<std::size_t>::max())
    return Status{StatusCode::kCorruption, "snapshot count overflow"};
  if (count > (bytes.size() - position) / 16U)
    return Status{StatusCode::kCorruption, "snapshot count exceeds payload"};
  entries.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t index = 0; index < count; ++index) {
    std::uint64_t key_size = 0;
    std::uint64_t value_size = 0;
    if (!Get(bytes, position, key_size) || !Get(bytes, position, value_size) ||
        key_size > std::numeric_limits<std::size_t>::max() ||
        value_size > std::numeric_limits<std::size_t>::max() ||
        key_size > bytes.size() - position || value_size > bytes.size() - position - key_size) {
      return Status{StatusCode::kCorruption, "snapshot record bounds"};
    }
    const auto key_length = static_cast<std::size_t>(key_size);
    const auto value_length = static_cast<std::size_t>(value_size);
    entries.push_back(
        {bytes.substr(position, key_length), bytes.substr(position + key_length, value_length)});
    position += key_length + value_length;
  }
  if (position != bytes.size()) return Status{StatusCode::kCorruption, "snapshot trailing bytes"};
  const auto status = engine.Import(entries);
  return status.ok() ? Result<std::uint64_t>(offset) : Result<std::uint64_t>(status);
}

}  // namespace kvstore
