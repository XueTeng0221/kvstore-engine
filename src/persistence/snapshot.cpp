#include "kvstore/persistence/snapshot.hpp"

#include <fcntl.h>
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
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
constexpr std::uintmax_t kMaxSnapshotBytes = 1ULL << 30U;
constexpr std::uint64_t kMaxSnapshotRecords = 10'000'000;
constexpr std::size_t kSnapshotHeaderBytes = 52;

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

Status SubmitIoUring(int ring_fd, io_uring_params& params, void* sq_ring, void* cq_ring,
                     io_uring_sqe* sqes, std::uint8_t opcode, int file_fd, const void* data,
                     std::size_t size) {
  auto* sq_tail = reinterpret_cast<unsigned*>(static_cast<char*>(sq_ring) + params.sq_off.tail);
  auto* sq_mask =
      reinterpret_cast<unsigned*>(static_cast<char*>(sq_ring) + params.sq_off.ring_mask);
  auto* sq_array = reinterpret_cast<unsigned*>(static_cast<char*>(sq_ring) + params.sq_off.array);
  const unsigned tail = std::atomic_ref<unsigned>(*sq_tail).load(std::memory_order_relaxed);
  const unsigned index = tail & *sq_mask;
  auto& request = sqes[index];
  std::memset(&request, 0, sizeof(request));
  request.opcode = opcode;
  request.fd = file_fd;
  request.off = 0;
  request.addr = reinterpret_cast<std::uint64_t>(data);
  request.len = static_cast<std::uint32_t>(size);
  sq_array[index] = index;
  std::atomic_ref<unsigned>(*sq_tail).store(tail + 1U, std::memory_order_release);
  if (::syscall(__NR_io_uring_enter, ring_fd, 1U, 1U, IORING_ENTER_GETEVENTS, nullptr, 0U) < 0)
    return {StatusCode::kIoError, "io_uring submit failed"};
  auto* cq_head = reinterpret_cast<unsigned*>(static_cast<char*>(cq_ring) + params.cq_off.head);
  auto* cq_tail = reinterpret_cast<unsigned*>(static_cast<char*>(cq_ring) + params.cq_off.tail);
  auto* cq_mask =
      reinterpret_cast<unsigned*>(static_cast<char*>(cq_ring) + params.cq_off.ring_mask);
  auto* completions =
      reinterpret_cast<io_uring_cqe*>(static_cast<char*>(cq_ring) + params.cq_off.cqes);
  const unsigned head = std::atomic_ref<unsigned>(*cq_head).load(std::memory_order_acquire);
  if (head == std::atomic_ref<unsigned>(*cq_tail).load(std::memory_order_acquire))
    return {StatusCode::kIoError, "io_uring completion missing"};
  const int result = completions[head & *cq_mask].res;
  std::atomic_ref<unsigned>(*cq_head).store(head + 1U, std::memory_order_release);
  const int expected = opcode == IORING_OP_WRITE ? static_cast<int>(size) : 0;
  return result == expected ? Status::Ok()
                            : Status{StatusCode::kIoError, "io_uring operation failed"};
}

Status WriteWithIoUring(int file_fd, std::string_view bytes) {
  if (bytes.size() > std::numeric_limits<std::uint32_t>::max())
    return {StatusCode::kLimitExceeded, "io_uring snapshot write limit"};
  io_uring_params params{};
  const int raw_ring_fd = static_cast<int>(::syscall(__NR_io_uring_setup, 2U, &params));
  if (raw_ring_fd < 0) return {StatusCode::kUnsupported, "io_uring unavailable"};
  const UniqueFd ring_fd(raw_ring_fd);
  const auto sq_size = params.sq_off.array + params.sq_entries * sizeof(unsigned);
  const auto cq_size = params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);
  const bool shared = (params.features & IORING_FEAT_SINGLE_MMAP) != 0;
  const auto ring_size = shared ? std::max(sq_size, cq_size) : sq_size;
  MappedRegion sq_ring(::mmap(nullptr, ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                              ring_fd.get(), IORING_OFF_SQ_RING),
                       ring_size);
  if (!sq_ring) return {StatusCode::kIoError, "io_uring SQ mmap failed"};
  MappedRegion cq_mapping;
  void* cq_ring = sq_ring.data();
  if (!shared) {
    cq_mapping = MappedRegion(::mmap(nullptr, cq_size, PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_POPULATE, ring_fd.get(), IORING_OFF_CQ_RING),
                              cq_size);
    if (!cq_mapping) return {StatusCode::kIoError, "io_uring CQ mmap failed"};
    cq_ring = cq_mapping.data();
  }
  const auto sqes_size = params.sq_entries * sizeof(io_uring_sqe);
  MappedRegion sqes_mapping(::mmap(nullptr, sqes_size, PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_POPULATE, ring_fd.get(), IORING_OFF_SQES),
                            sqes_size);
  if (!sqes_mapping) return {StatusCode::kIoError, "io_uring SQE mmap failed"};
  auto* sqes = static_cast<io_uring_sqe*>(sqes_mapping.data());
  auto status = SubmitIoUring(ring_fd.get(), params, sq_ring.data(), cq_ring, sqes, IORING_OP_WRITE,
                              file_fd, bytes.data(), bytes.size());
  if (!status.ok()) return status;
  return SubmitIoUring(ring_fd.get(), params, sq_ring.data(), cq_ring, sqes, IORING_OP_FSYNC,
                       file_fd, nullptr, 0);
}

}  // namespace

Status Snapshot::Save(const std::filesystem::path& directory, const IEngine& engine,
                      std::uint64_t last_offset, std::uint64_t last_event_id, bool io_uring_write,
                      bool allow_sync_fallback) {
  const auto entries = engine.Export();
  if (!entries.ok()) return entries.status();
  if (entries.value().size() > kMaxSnapshotRecords)
    return {StatusCode::kLimitExceeded, "snapshot record limit"};
  std::string payload;
  for (const auto& entry : entries.value()) {
    const auto record_bytes = 16U + entry.key.size() + entry.value.size();
    if (record_bytes > kMaxSnapshotBytes - kSnapshotHeaderBytes ||
        payload.size() > kMaxSnapshotBytes - kSnapshotHeaderBytes - record_bytes)
      return {StatusCode::kLimitExceeded, "snapshot size limit"};
    Put<std::uint64_t>(payload, entry.key.size());
    Put<std::uint64_t>(payload, entry.value.size());
    payload.append(entry.key);
    payload.append(entry.value);
  }
  std::string header(kMagic.data(), kMagic.size());
  Put<std::uint32_t>(header, kVersion);
  Put<std::uint32_t>(header, 0U);
  Put<std::uint64_t>(header, entries.value().size());
  Put<std::uint64_t>(header, last_offset);
  Put<std::uint64_t>(header, last_event_id);
  Put<std::uint64_t>(header, payload.size());
  std::string crc_input = header + payload;
  const auto crc = Crc32(std::as_bytes(std::span(crc_input.data(), crc_input.size())));
  Put<std::uint32_t>(header, crc);
  std::filesystem::create_directories(directory);
  const auto temporary = directory / "snapshot.tmp";
  const auto final = directory / "snapshot.kvs";
  const UniqueFd file_fd(::open(temporary.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644));
  if (!file_fd) return {StatusCode::kIoError, "cannot create snapshot"};
  const std::string bytes = header + payload;
  auto write_status = io_uring_write ? WriteWithIoUring(file_fd.get(), bytes)
                                     : Status{StatusCode::kUnsupported, "sync writer selected"};
  if (!io_uring_write || (!write_status.ok() && allow_sync_fallback)) {
    if (::lseek(file_fd.get(), 0, SEEK_SET) < 0 || ::ftruncate(file_fd.get(), 0) < 0)
      return {StatusCode::kIoError, "snapshot fallback reset failed"};
    std::size_t written = 0;
    while (written < bytes.size()) {
      const auto count = ::write(file_fd.get(), bytes.data() + written, bytes.size() - written);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return {StatusCode::kIoError, "cannot write snapshot"};
      written += static_cast<std::size_t>(count);
    }
    if (::fsync(file_fd.get()) < 0) return {StatusCode::kIoError, "snapshot fsync failed"};
  } else if (!write_status.ok()) {
    return write_status;
  }
  try {
    std::filesystem::rename(temporary, final);
  } catch (const std::filesystem::filesystem_error&) {
    return {StatusCode::kIoError, "snapshot rename failed"};
  }
  const UniqueFd directory_fd(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!directory_fd || ::fsync(directory_fd.get()) < 0)
    return {StatusCode::kIoError, "snapshot directory fsync failed"};
  return Status::Ok();
}

Result<RecoveryPoint> Snapshot::LoadWithMetadata(const std::filesystem::path& directory,
                                                 IEngine& engine) {
  const auto path = directory / "snapshot.kvs";
  const UniqueFd file_fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (!file_fd) {
    if (errno == ENOENT) return RecoveryPoint{};
    return Status{StatusCode::kIoError, "cannot open snapshot"};
  }
  struct stat file_stat {};
  if (::fstat(file_fd.get(), &file_stat) < 0) return Status{StatusCode::kIoError, "snapshot stat"};
  if (file_stat.st_size < 44) return Status{StatusCode::kCorruption, "invalid snapshot size"};
  const auto size = static_cast<std::uintmax_t>(file_stat.st_size);
  if (size < 32 || size > kMaxSnapshotBytes || size > std::numeric_limits<std::size_t>::max()) {
    return Status{StatusCode::kCorruption, "invalid snapshot size"};
  }
  const MappedRegion mapping(
      ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ, MAP_PRIVATE, file_fd.get(), 0),
      static_cast<std::size_t>(size));
  if (!mapping) return Status{StatusCode::kIoError, "snapshot mmap"};
  const std::string_view bytes(static_cast<const char*>(mapping.data()), mapping.size());
  if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
    return Status{StatusCode::kCorruption, "snapshot magic"};
  std::size_t position = kMagic.size();
  std::uint32_t version = 0;
  std::uint32_t flags = 0;
  std::uint64_t count = 0;
  std::uint64_t offset = 0;
  std::uint64_t event_id = 0;
  std::uint64_t payload_size = 0;
  std::uint32_t expected_crc = 0;
  if (!Get(bytes, position, version) || !Get(bytes, position, flags) ||
      !Get(bytes, position, count) || !Get(bytes, position, offset) ||
      !Get(bytes, position, event_id) || !Get(bytes, position, payload_size) ||
      !Get(bytes, position, expected_crc) || version != kVersion || flags != 0 ||
      payload_size != bytes.size() - position) {
    return Status{StatusCode::kCorruption, "snapshot header"};
  }
  const std::array<ByteView, 2> crc_parts{
      std::as_bytes(std::span(bytes.data(), position - sizeof(std::uint32_t))),
      std::as_bytes(std::span(bytes.data() + position, static_cast<std::size_t>(payload_size)))};
  const auto actual_crc = Crc32Parts(crc_parts);
  if (actual_crc != expected_crc) return Status{StatusCode::kCorruption, "snapshot checksum"};
  std::vector<Entry> entries;
  if (count > kMaxSnapshotRecords || count > engine.Capacity() ||
      count > std::numeric_limits<std::size_t>::max())
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
    entries.push_back({std::string(bytes.substr(position, key_length)),
                       std::string(bytes.substr(position + key_length, value_length))});
    position += key_length + value_length;
  }
  if (position != bytes.size()) return Status{StatusCode::kCorruption, "snapshot trailing bytes"};
  const auto status = engine.Import(entries);
  return status.ok() ? Result<RecoveryPoint>(RecoveryPoint{offset, event_id})
                     : Result<RecoveryPoint>(status);
}

Result<std::uint64_t> Snapshot::Load(const std::filesystem::path& directory, IEngine& engine) {
  auto result = LoadWithMetadata(directory, engine);
  if (!result.ok()) return result.status();
  return result.value().offset;
}

}  // namespace kvstore
