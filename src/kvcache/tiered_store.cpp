#include "kvstore/kvcache/tiered_store.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

#include "kvstore/common/crc32.hpp"
#include "kvstore/common/resources.hpp"

namespace kvstore::kvcache {
namespace {

constexpr std::array<std::byte, 4> kMetadataMagic{std::byte{'K'}, std::byte{'V'}, std::byte{'D'},
                                                  std::byte{'1'}};
constexpr std::uint16_t kMetadataVersion = 1;
constexpr std::size_t kMetadataFixedBytes = 4U + 2U + 2U + 8U + 8U + 8U + 4U + 4U + 32U + 4U;

std::size_t AlignedSize(std::size_t size) {
  return (size + kChunkAlignmentBytes - 1U) &
         ~(static_cast<std::size_t>(kChunkAlignmentBytes) - 1U);
}

void PutU16(Bytes& output, std::uint16_t value) {
  output.push_back(static_cast<std::byte>(value >> 8U));
  output.push_back(static_cast<std::byte>(value));
}

void PutU32(Bytes& output, std::uint32_t value) {
  for (unsigned shift = 24; shift <= 24; shift -= 8) {
    output.push_back(static_cast<std::byte>(value >> shift));
  }
}

void PutU64(Bytes& output, std::uint64_t value) {
  for (unsigned shift = 56; shift <= 56; shift -= 8) {
    output.push_back(static_cast<std::byte>(value >> shift));
  }
}

template <typename T>
bool GetInteger(ByteView input, std::size_t& position, T& value) {
  if (position > input.size() || sizeof(T) > input.size() - position) return false;
  value = 0;
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    value = static_cast<T>((value << 8U) | std::to_integer<std::uint8_t>(input[position++]));
  }
  return true;
}

Status InvokeFault(const std::function<Status(std::string_view)>& fault, std::string_view stage) {
  if (!fault) return Status::Ok();
  try {
    return fault(stage);
  } catch (...) {
    return {StatusCode::kInternal, "I/O fault hook threw an exception"};
  }
}

Status WriteAll(int fd, ByteView bytes, const std::function<Status(std::string_view)>& fault,
                std::string_view kind) {
  std::size_t position = 0;
  bool injected = false;
  while (position < bytes.size()) {
    const std::size_t requested =
        !injected && bytes.size() - position > 17U ? 17U : bytes.size() - position;
    const auto written = ::write(fd, bytes.data() + position, requested);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return {StatusCode::kIoError, "disk chunk write failed"};
    position += static_cast<std::size_t>(written);
    if (!injected && position < bytes.size()) {
      injected = true;
      const Status status = InvokeFault(
          fault, kind == "chunk" ? "chunk.write.after-progress" : "metadata.write.after-progress");
      if (!status.ok()) return status;
    }
  }
  return Status::Ok();
}

Status ReadAll(int fd, std::span<std::byte> bytes,
               const std::function<Status(std::string_view)>& fault, std::string_view kind) {
  std::size_t position = 0;
  bool injected = false;
  while (position < bytes.size()) {
    const std::size_t requested =
        !injected && bytes.size() - position > 17U ? 17U : bytes.size() - position;
    const auto count = ::read(fd, bytes.data() + position, requested);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) return {StatusCode::kIoError, "disk chunk read failed"};
    if (count == 0) return {StatusCode::kCorruption, "disk chunk is truncated"};
    position += static_cast<std::size_t>(count);
    if (!injected && position < bytes.size()) {
      injected = true;
      const Status status = InvokeFault(
          fault, kind == "chunk" ? "chunk.read.after-progress" : "metadata.read.after-progress");
      if (!status.ok()) return status;
    }
  }
  std::byte extra{};
  while (true) {
    const auto count = ::read(fd, &extra, 1);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) return {StatusCode::kIoError, "disk chunk trailing-byte check failed"};
    if (count != 0) return {StatusCode::kCorruption, "disk chunk has trailing bytes"};
    break;
  }
  return Status::Ok();
}

Status SyncDirectory(const std::filesystem::path& path) {
  const UniqueFd fd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!fd || ::fsync(fd.get()) < 0) return {StatusCode::kIoError, "disk directory fsync failed"};
  return Status::Ok();
}

Status SyncDirectory(const std::filesystem::path& path,
                     const std::function<Status(std::string_view)>& fault, std::string_view stage) {
  const Status injected = InvokeFault(fault, stage);
  if (!injected.ok()) return injected;
  return SyncDirectory(path);
}

Status PublishFile(const std::filesystem::path& temporary, const std::filesystem::path& final,
                   ByteView bytes, const std::function<Status(std::string_view)>& fault,
                   std::string_view kind, bool* renamed = nullptr) {
  const UniqueFd fd(
      ::open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0644));
  if (!fd) return {StatusCode::kIoError, "cannot create temporary disk file"};
  Status status = InvokeFault(fault, kind == "chunk" ? "chunk.write" : "metadata.write");
  if (status.ok()) status = WriteAll(fd.get(), bytes, fault, kind);
  if (status.ok()) status = InvokeFault(fault, kind == "chunk" ? "chunk.fsync" : "metadata.fsync");
  if (!status.ok() || ::fsync(fd.get()) < 0) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return status.ok() ? Status{StatusCode::kIoError, "disk file fsync failed"} : status;
  }
  status = InvokeFault(fault, kind == "chunk" ? "chunk.rename" : "metadata.rename");
  if (!status.ok()) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return status;
  }
  std::error_code error;
  std::filesystem::rename(temporary, final, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return {StatusCode::kIoError, "disk file rename failed"};
  }
  if (renamed != nullptr) *renamed = true;
  return SyncDirectory(final.parent_path(), fault,
                       kind == "chunk" ? "chunk.dirsync" : "metadata.dirsync");
}

Result<Bytes> ReadFile(const std::filesystem::path& path, std::uint64_t expected_size,
                       std::uint64_t maximum_size,
                       const std::function<Status(std::string_view)>& fault = {},
                       std::string_view kind = "metadata") try {
  if (expected_size > maximum_size || expected_size > std::numeric_limits<std::size_t>::max()) {
    return Status{StatusCode::kCorruption, "disk file size exceeds configured limit"};
  }
  const UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (!fd) return Status{StatusCode::kIoError, "cannot open disk file"};
  struct stat file_status {};
  if (::fstat(fd.get(), &file_status) < 0 || file_status.st_size < 0 ||
      static_cast<std::uint64_t>(file_status.st_size) != expected_size) {
    return Status{StatusCode::kCorruption, "disk file length mismatch"};
  }
  Bytes bytes(static_cast<std::size_t>(expected_size));
  Status status = ReadAll(fd.get(), bytes, fault, kind);
  return status.ok() ? Result<Bytes>(std::move(bytes)) : Result<Bytes>(status);
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "unable to allocate disk read buffer"};
} catch (const std::filesystem::filesystem_error&) {
  return Status{StatusCode::kIoError, "disk file lookup failed"};
}

std::string ObjectHex(const CacheKey& key) { return DigestHex(key.digest); }

std::filesystem::path ObjectPath(const std::filesystem::path& root, const CacheKey& key) {
  const std::string hex = ObjectHex(key);
  return root / "objects" / hex.substr(0, 2) / hex;
}

Status RequireRealDirectory(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) {
    return {StatusCode::kIoError, "tiered store path is not a real directory"};
  }
  return Status::Ok();
}

Status ValidateExistingPathComponents(const std::filesystem::path& path) {
  std::filesystem::path current;
  std::error_code error;
  for (const auto& component : path) {
    current /= component;
    if (!std::filesystem::exists(current, error)) {
      if (error) return {StatusCode::kIoError, "tiered store path inspection failed"};
      continue;
    }
    const auto status = std::filesystem::symlink_status(current, error);
    if (error || std::filesystem::is_symlink(status)) {
      return {StatusCode::kIoError, "tiered store path contains a symbolic link"};
    }
  }
  return Status::Ok();
}

struct DiskChunk {
  std::uint64_t size{};
  std::uint32_t crc{};
  Digest sha{};
};

struct DiskRecord {
  Bytes canonical;
  Digest payload_digest{};
  std::uint64_t payload_bytes{};
  std::uint64_t created_at_ns{};
  std::uint64_t accessed_at_ns{};
  std::vector<DiskChunk> chunks;
  std::uint64_t disk_bytes{};
};

Result<Bytes> EncodeMetadata(const DiskRecord& record) try {
  if (record.canonical.size() > std::numeric_limits<std::uint32_t>::max() ||
      record.chunks.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Status{StatusCode::kLimitExceeded, "disk metadata field limit exceeded"};
  }
  Bytes output;
  const std::size_t chunk_bytes = record.chunks.size() * (8U + 4U + kDigestBytes);
  if (record.canonical.size() >
      std::numeric_limits<std::size_t>::max() - kMetadataFixedBytes - chunk_bytes) {
    return Status{StatusCode::kLimitExceeded, "disk metadata size overflows"};
  }
  output.reserve(kMetadataFixedBytes + chunk_bytes + record.canonical.size());
  output.insert(output.end(), kMetadataMagic.begin(), kMetadataMagic.end());
  PutU16(output, kMetadataVersion);
  PutU16(output, 0);
  PutU64(output, record.payload_bytes);
  PutU64(output, record.created_at_ns);
  PutU64(output, record.accessed_at_ns);
  PutU32(output, static_cast<std::uint32_t>(record.chunks.size()));
  PutU32(output, static_cast<std::uint32_t>(record.canonical.size()));
  output.insert(output.end(), record.payload_digest.begin(), record.payload_digest.end());
  for (const auto& chunk : record.chunks) {
    PutU64(output, chunk.size);
    PutU32(output, chunk.crc);
    output.insert(output.end(), chunk.sha.begin(), chunk.sha.end());
  }
  output.insert(output.end(), record.canonical.begin(), record.canonical.end());
  PutU32(output, Crc32(output));
  return output;
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "unable to allocate disk metadata"};
}

Result<DiskRecord> DecodeMetadata(ByteView bytes, const TieredStoreConfig& config) try {
  if (bytes.size() < kMetadataFixedBytes || bytes.size() > config.max_metadata_bytes ||
      !std::equal(kMetadataMagic.begin(), kMetadataMagic.end(), bytes.begin())) {
    return Status{StatusCode::kCorruption, "invalid disk metadata header"};
  }
  const std::uint32_t stored_crc =
      (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[bytes.size() - 4])) << 24U) |
      (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[bytes.size() - 3])) << 16U) |
      (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[bytes.size() - 2])) << 8U) |
      static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[bytes.size() - 1]));
  if (Crc32(bytes.first(bytes.size() - 4U)) != stored_crc) {
    return Status{StatusCode::kCorruption, "disk metadata checksum mismatch"};
  }
  std::size_t position = kMetadataMagic.size();
  std::uint16_t version = 0;
  std::uint16_t flags = 0;
  std::uint32_t count = 0;
  std::uint32_t canonical_size = 0;
  DiskRecord record;
  if (!GetInteger(bytes, position, version) || !GetInteger(bytes, position, flags) ||
      !GetInteger(bytes, position, record.payload_bytes) ||
      !GetInteger(bytes, position, record.created_at_ns) ||
      !GetInteger(bytes, position, record.accessed_at_ns) || !GetInteger(bytes, position, count) ||
      !GetInteger(bytes, position, canonical_size) || version != kMetadataVersion || flags != 0 ||
      count == 0 || count > config.max_chunks_per_object || record.payload_bytes == 0 ||
      record.payload_bytes > config.max_object_bytes) {
    return Status{StatusCode::kCorruption, "invalid disk metadata fields"};
  }
  if (position > bytes.size() || kDigestBytes > bytes.size() - position) {
    return Status{StatusCode::kCorruption, "truncated disk payload digest"};
  }
  std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(position), kDigestBytes,
              record.payload_digest.begin());
  position += kDigestBytes;
  constexpr std::size_t kChunkRecordBytes = 8U + 4U + kDigestBytes;
  if (count > (bytes.size() - position) / kChunkRecordBytes) {
    return Status{StatusCode::kCorruption, "disk chunk count exceeds metadata"};
  }
  record.chunks.resize(count);
  std::uint64_t total = 0;
  for (auto& chunk : record.chunks) {
    if (!GetInteger(bytes, position, chunk.size) || !GetInteger(bytes, position, chunk.crc) ||
        chunk.size == 0 || chunk.size > config.max_object_bytes || position > bytes.size() ||
        kDigestBytes > bytes.size() - position ||
        chunk.size > config.max_object_bytes - std::min(config.max_object_bytes, total)) {
      return Status{StatusCode::kCorruption, "invalid disk chunk metadata"};
    }
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(position), kDigestBytes,
                chunk.sha.begin());
    position += kDigestBytes;
    total += chunk.size;
  }
  if (total != record.payload_bytes || canonical_size > config.max_metadata_bytes ||
      position > bytes.size() || bytes.size() - position < 4U ||
      canonical_size != bytes.size() - position - 4U) {
    return Status{StatusCode::kCorruption, "disk metadata length mismatch"};
  }
  record.canonical.assign(bytes.begin() + static_cast<std::ptrdiff_t>(position), bytes.end() - 4);
  record.disk_bytes = static_cast<std::uint64_t>(bytes.size()) + total;
  return record;
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "unable to allocate decoded disk metadata"};
}

}  // namespace

Status ValidateTierTransition(TierState from, TierState to) {
  const bool valid =
      (from == TierState::kResident && (to == TierState::kEvicting || to == TierState::kFailed)) ||
      (from == TierState::kEvicting &&
       (to == TierState::kDiskOnly || to == TierState::kResident || to == TierState::kFailed)) ||
      (from == TierState::kDiskOnly &&
       (to == TierState::kLoading || to == TierState::kEvicting || to == TierState::kFailed)) ||
      (from == TierState::kLoading &&
       (to == TierState::kResident || to == TierState::kDiskOnly || to == TierState::kFailed)) ||
      (from == TierState::kFailed &&
       (to == TierState::kLoading || to == TierState::kEvicting || to == TierState::kDiskOnly));
  return valid ? Status::Ok()
               : Status{StatusCode::kInvalidArgument, "illegal tier state transition"};
}

struct ResidentPool::State {
  explicit State(ResidentPoolConfig value) : config(value) {}
  ~State() {
    for (auto& [capacity, blocks] : free_blocks) {
      for (std::byte* block : blocks) {
        ::operator delete(block, std::align_val_t{kChunkAlignmentBytes});
      }
    }
  }
  ResidentPoolConfig config;
  std::mutex mutex;
  std::unordered_map<std::size_t, std::vector<std::byte*>> free_blocks;
  std::atomic<std::uint64_t> used{};
  std::atomic<std::uint64_t> requested{};
  std::atomic<std::uint64_t> peak{};
  std::atomic<std::uint64_t> count{};
};

struct ResidentAllocation::Block {
  Block(std::shared_ptr<ResidentPool::State> owner_value, std::byte* data_value,
        std::size_t size_value, std::size_t capacity_value)
      : owner(std::move(owner_value)),
        data(data_value),
        size(size_value),
        capacity(capacity_value) {}
  ~Block() {
    bool retained = false;
    try {
      std::lock_guard lock(owner->mutex);
      owner->free_blocks[capacity].push_back(data);
      retained = true;
    } catch (...) {
      // Destruction must never terminate when the free-list cannot grow.
    }
    if (!retained) {
      ::operator delete(data, std::align_val_t{kChunkAlignmentBytes});
      owner->used.fetch_sub(capacity, std::memory_order_relaxed);
    }
    owner->requested.fetch_sub(size, std::memory_order_relaxed);
    owner->count.fetch_sub(1, std::memory_order_relaxed);
  }
  std::shared_ptr<ResidentPool::State> owner;
  std::byte* data;
  std::size_t size;
  std::size_t capacity;
};

ResidentAllocation::ResidentAllocation(std::shared_ptr<Block> block) : block_(std::move(block)) {}
ByteView ResidentAllocation::bytes() const noexcept {
  return block_ == nullptr ? ByteView{} : ByteView{block_->data, block_->size};
}
std::size_t ResidentAllocation::size() const noexcept {
  return block_ == nullptr ? 0 : block_->size;
}
std::uintptr_t ResidentAllocation::address() const noexcept {
  return block_ == nullptr ? 0 : reinterpret_cast<std::uintptr_t>(block_->data);
}

ResidentPool::ResidentPool(ResidentPoolConfig config) : state_(std::make_shared<State>(config)) {}

Status ResidentPool::Validate() const {
  const auto& config = state_->config;
  if (config.budget_bytes == 0 || config.low_watermark_bytes > config.high_watermark_bytes ||
      config.high_watermark_bytes > config.budget_bytes) {
    return {StatusCode::kInvalidArgument, "invalid resident pool budget or watermarks"};
  }
  return Status::Ok();
}

Result<ResidentAllocation> ResidentPool::Copy(ByteView bytes) try {
  const Status validation = Validate();
  if (!validation.ok()) return validation;
  if (bytes.empty() || bytes.size() > std::numeric_limits<std::size_t>::max() - 63U) {
    return Status{StatusCode::kInvalidArgument, "invalid resident allocation size"};
  }
  const std::size_t capacity = AlignedSize(bytes.size());
  std::byte* data = nullptr;
  bool charged = false;
  bool reused = false;
  try {
    std::lock_guard lock(state_->mutex);
    auto& reusable = state_->free_blocks[capacity];
    if (!reusable.empty()) {
      data = reusable.back();
      reusable.pop_back();
      reused = true;
    } else {
      std::uint64_t current = state_->used.load(std::memory_order_relaxed);
      if (current > state_->config.budget_bytes ||
          capacity > state_->config.budget_bytes - current) {
        return Status{StatusCode::kLimitExceeded, "resident memory budget exceeded"};
      }
      data =
          static_cast<std::byte*>(::operator new(capacity, std::align_val_t{kChunkAlignmentBytes}));
      state_->used.fetch_add(capacity, std::memory_order_relaxed);
      charged = true;
      std::uint64_t peak = state_->peak.load(std::memory_order_relaxed);
      const std::uint64_t updated = current + capacity;
      while (peak < updated &&
             !state_->peak.compare_exchange_weak(peak, updated, std::memory_order_relaxed)) {
      }
    }
    std::copy(bytes.begin(), bytes.end(), data);
    auto block = std::make_shared<ResidentAllocation::Block>(state_, data, bytes.size(), capacity);
    data = nullptr;
    state_->requested.fetch_add(bytes.size(), std::memory_order_relaxed);
    state_->count.fetch_add(1, std::memory_order_relaxed);
    return ResidentAllocation(std::move(block));
  } catch (...) {
    if (data != nullptr) {
      if (reused) {
        ::operator delete(data, std::align_val_t{kChunkAlignmentBytes});
        state_->used.fetch_sub(capacity, std::memory_order_relaxed);
      } else {
        ::operator delete(data, std::align_val_t{kChunkAlignmentBytes});
      }
    }
    if (charged) state_->used.fetch_sub(capacity, std::memory_order_relaxed);
    throw;
  }
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "resident allocation failed"};
} catch (...) {
  return Status{StatusCode::kInternal, "resident allocation failed"};
}

ResidentPoolStats ResidentPool::Stats() const noexcept {
  const std::uint64_t used = state_->used.load(std::memory_order_relaxed);
  const std::uint64_t requested = state_->requested.load(std::memory_order_relaxed);
  return {state_->config.budget_bytes,
          used,
          requested,
          used - std::min(used, requested),
          state_->peak.load(std::memory_order_relaxed),
          state_->count.load(std::memory_order_relaxed),
          used >= state_->config.high_watermark_bytes,
          used <= state_->config.low_watermark_bytes};
}

struct TieredResidentHandle::Pin {
  TensorManifest manifest;
  CacheKey key;
  std::vector<ResidentAllocation> chunks;
  std::shared_ptr<const TensorRangeView> view;
};

TieredResidentHandle::TieredResidentHandle(std::shared_ptr<const Pin> pin) : pin_(std::move(pin)) {}
const TensorManifest& TieredResidentHandle::manifest() const noexcept { return pin_->manifest; }
const CacheKey& TieredResidentHandle::key() const noexcept { return pin_->key; }
std::size_t TieredResidentHandle::chunk_count() const noexcept { return pin_->chunks.size(); }
const TensorRangeView* TieredResidentHandle::range_view() const noexcept { return pin_->view.get(); }
std::uint32_t TieredResidentHandle::physical_chunk_index(std::size_t index) const {
  return pin_->view == nullptr ? static_cast<std::uint32_t>(index) : pin_->view->physical_chunk_indices.at(index);
}
Result<ByteView> TieredResidentHandle::Chunk(std::size_t index) const {
  if (index >= pin_->chunks.size()) {
    return Status{StatusCode::kInvalidArgument, "chunk index is out of range"};
  }
  return pin_->chunks[index].bytes();
}

struct TieredStore::Impl {
  struct Entry {
    TierState state{TierState::kDiskOnly};
    CacheKey key;
    Bytes canonical;
    Digest payload_digest{};
    DiskRecord disk;
    std::shared_ptr<const TieredResidentHandle::Pin> resident;
    Status last_error;
    std::condition_variable_any changed;
  };

  explicit Impl(TieredStoreConfig value) : config(std::move(value)), pool(config.resident) {}
  TieredStoreConfig config;
  ResidentPool pool;
  mutable std::recursive_mutex mutex;
  std::unordered_map<std::string, std::shared_ptr<Entry>> entries;
  std::uint64_t disk_used{};
  std::uint64_t disk_reserved{};
  std::atomic<std::uint64_t> disk_reads{};

  Result<std::shared_ptr<const TieredResidentHandle::Pin>> Load(const std::shared_ptr<Entry>& entry,
                                                                const TensorManifest& manifest) {
    std::vector<ResidentAllocation> allocations;
    try {
      allocations.reserve(entry->disk.chunks.size());
      if (entry->disk.payload_bytes > std::numeric_limits<std::size_t>::max()) {
        return Status{StatusCode::kLimitExceeded, "promoted payload size overflows"};
      }
      const auto directory = ObjectPath(config.directory, entry->key);
      disk_reads.fetch_add(1, std::memory_order_relaxed);
      for (const auto& chunk : entry->disk.chunks) {
        auto bytes = ReadFile(directory / DigestHex(chunk.sha), chunk.size, config.max_object_bytes,
                              config.io_fault, "chunk");
        if (!bytes.ok()) return bytes.status();
        if (Crc32(bytes.value()) != chunk.crc) {
          return Status{StatusCode::kCorruption, "disk chunk CRC mismatch"};
        }
        auto digest = Sha256(bytes.value());
        if (!digest.ok()) return digest.status();
        if (digest.value() != chunk.sha) {
          return Status{StatusCode::kCorruption, "disk chunk SHA-256 mismatch"};
        }
        auto allocation = pool.Copy(bytes.value());
        if (!allocation.ok()) return allocation.status();
        allocations.push_back(std::move(allocation.value()));
      }
      std::vector<ByteView> payload_parts;
      payload_parts.reserve(allocations.size());
      for (const ResidentAllocation& allocation : allocations) {
        payload_parts.push_back(allocation.bytes());
      }
      auto payload_digest = Sha256Parts(payload_parts);
      if (!payload_digest.ok()) return payload_digest.status();
      if (payload_digest.value() != entry->payload_digest) {
        return Status{StatusCode::kCorruption, "disk payload SHA-256 mismatch"};
      }
      TensorManifest persisted_manifest = manifest;
      persisted_manifest.payload_digest = entry->payload_digest;
      persisted_manifest.created_at_ns = entry->disk.created_at_ns;
      persisted_manifest.accessed_at_ns = entry->disk.accessed_at_ns;
      auto pin = std::make_shared<TieredResidentHandle::Pin>(TieredResidentHandle::Pin{
          std::move(persisted_manifest), entry->key, std::move(allocations), nullptr});
      return std::shared_ptr<const TieredResidentHandle::Pin>(std::move(pin));
    } catch (const std::bad_alloc&) {
      return Status{StatusCode::kLimitExceeded, "unable to allocate promoted object"};
    } catch (const std::filesystem::filesystem_error&) {
      return Status{StatusCode::kIoError, "disk chunk path failed"};
    }
  }

  Result<std::shared_ptr<const TieredResidentHandle::Pin>> LoadRanges(
      const std::shared_ptr<Entry>& entry, const TensorManifest& manifest,
      std::span<const TensorRange> ranges) {
    if (ranges.empty() || manifest.chunk_bytes == 0 || manifest.payload_bytes == 0) {
      return Status{StatusCode::kInvalidArgument, "tiered load ranges are empty or invalid"};
    }
    if (manifest.layout != TensorLayout::kLayerMajor ||
        manifest.key_value_packing != KeyValuePacking::kPlanar) {
      return Status{StatusCode::kUnsupported, "tiered range loading does not support this layout"};
    }
    const auto axis_index = [&manifest](TensorAxis axis) -> Result<std::size_t> {
      const auto iterator = std::ranges::find(manifest.axis_order, axis);
      if (iterator == manifest.axis_order.end()) {
        return Status{StatusCode::kInvalidArgument, "manifest is missing a required axis"};
      }
      return static_cast<std::size_t>(std::distance(manifest.axis_order.begin(), iterator));
    };
    const auto layer_axis = axis_index(TensorAxis::kLayer);
    const auto token_axis = axis_index(TensorAxis::kToken);
    if (!layer_axis.ok() || !token_axis.ok() || manifest.layer_count == 0 ||
        manifest.token_count == 0 ||
        manifest.layer_begin > std::numeric_limits<std::uint32_t>::max() - manifest.layer_count) {
      return Status{StatusCode::kInvalidArgument, "manifest lacks layer/token strides"};
    }
    const auto layer_stride = manifest.strides_bytes[layer_axis.value()];
    const auto token_stride = manifest.strides_bytes[token_axis.value()];
    const auto kv_axis = axis_index(TensorAxis::kKeyValue);
    if (layer_stride == 0 || token_stride == 0 || !kv_axis.ok() ||
        manifest.shape[kv_axis.value()] < 2 || manifest.strides_bytes[kv_axis.value()] == 0) {
      return Status{StatusCode::kInvalidArgument, "manifest has zero layer/token stride"};
    }
    std::vector<bool> selected(entry->disk.chunks.size());
    for (const auto& range : ranges) {
      if (range.token_begin >= range.token_end || range.token_end > manifest.token_count ||
          range.layer_begin >= range.layer_end || range.layer_begin < manifest.layer_begin ||
          range.layer_end > manifest.layer_begin + manifest.layer_count) {
        return Status{StatusCode::kInvalidArgument, "tensor range is outside manifest"};
      }
      const auto layer_offset = static_cast<std::uint64_t>(range.layer_begin - manifest.layer_begin);
      if (layer_offset > std::numeric_limits<std::uint64_t>::max() / layer_stride ||
          range.token_begin > std::numeric_limits<std::uint64_t>::max() / token_stride)
        return Status{StatusCode::kInvalidArgument, "tensor range offset overflows"};
      const auto kv_stride = manifest.strides_bytes[kv_axis.value()];
      for (std::uint64_t kv = 0; kv < 2; ++kv) {
        const auto first = kv * kv_stride + layer_offset * layer_stride + range.token_begin * token_stride;
        const auto last_layer = static_cast<std::uint64_t>(range.layer_end - manifest.layer_begin - 1);
        const auto last_token = range.token_end - 1;
        if (last_layer > std::numeric_limits<std::uint64_t>::max() / layer_stride ||
            last_token > std::numeric_limits<std::uint64_t>::max() / token_stride)
          return Status{StatusCode::kInvalidArgument, "tensor range end overflows"};
        const auto last_base = kv * kv_stride + last_layer * layer_stride + last_token * token_stride;
        if (last_base > std::numeric_limits<std::uint64_t>::max() - token_stride)
          return Status{StatusCode::kInvalidArgument, "tensor range end overflows"};
        const auto last = last_base + token_stride;
        if (first >= manifest.payload_bytes || last > manifest.payload_bytes || last < first)
          return Status{StatusCode::kInvalidArgument, "tensor range exceeds payload"};
        const auto first_chunk = first / manifest.chunk_bytes;
        const auto last_chunk = (last - 1) / manifest.chunk_bytes;
        if (last_chunk >= selected.size())
          return Status{StatusCode::kCorruption, "chunk metadata is incomplete"};
        for (std::uint64_t index = first_chunk; index <= last_chunk; ++index) selected[index] = true;
      }
    }
    std::vector<ResidentAllocation> allocations;
    for (std::uint32_t index = 0; index < selected.size(); ++index) {
      if (!selected[index]) continue;
      const auto& chunk = entry->disk.chunks[index];
      auto bytes = ReadFile(ObjectPath(config.directory, entry->key) / DigestHex(chunk.sha), chunk.size,
                            config.max_object_bytes, config.io_fault, "chunk");
      if (!bytes.ok()) return bytes.status();
      if (Crc32(bytes.value()) != chunk.crc) return Status{StatusCode::kCorruption, "disk chunk CRC mismatch"};
      auto digest = Sha256(bytes.value());
      if (!digest.ok()) return digest.status();
      if (digest.value() != chunk.sha) return Status{StatusCode::kCorruption, "disk chunk SHA-256 mismatch"};
      auto allocation = pool.Copy(bytes.value());
      if (!allocation.ok()) return allocation.status();
      allocations.push_back(std::move(allocation.value()));
    }
    auto view = std::make_shared<TensorRangeView>();
    view->ranges.assign(ranges.begin(), ranges.end());
    for (std::size_t index = 0; index < selected.size(); ++index)
      if (selected[index]) view->physical_chunk_indices.push_back(static_cast<std::uint32_t>(index));
    return std::shared_ptr<const TieredResidentHandle::Pin>(
        std::make_shared<TieredResidentHandle::Pin>(TieredResidentHandle::Pin{
            manifest, entry->key, std::move(allocations), std::move(view)}));
  }
};

TieredStore::TieredStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TieredStore::~TieredStore() = default;

Result<std::unique_ptr<TieredStore>> TieredStore::Open(TieredStoreConfig config) try {
  auto impl = std::make_unique<Impl>(std::move(config));
  const Status pool_status = impl->pool.Validate();
  if (!pool_status.ok() || impl->config.directory.empty() || impl->config.disk_budget_bytes == 0 ||
      impl->config.max_object_bytes == 0 || impl->config.max_chunks_per_object == 0 ||
      impl->config.max_metadata_bytes < kMetadataFixedBytes) {
    return pool_status.ok() ? Status{StatusCode::kInvalidArgument, "invalid tiered store config"}
                            : pool_status;
  }
  std::error_code error;
  const auto objects = impl->config.directory / "objects";
  Status path_status = ValidateExistingPathComponents(impl->config.directory);
  if (!path_status.ok()) return path_status;
  std::filesystem::create_directories(objects, error);
  if (error) return Status{StatusCode::kIoError, "cannot create tiered store directory"};
  path_status = RequireRealDirectory(impl->config.directory);
  if (!path_status.ok()) return path_status;
  path_status = RequireRealDirectory(objects);
  if (!path_status.ok()) return path_status;
  Status directory_status = SyncDirectory(impl->config.directory);
  if (!directory_status.ok()) return directory_status;
  const auto parent = impl->config.directory.parent_path();
  if (!parent.empty()) {
    directory_status = SyncDirectory(parent);
    if (!directory_status.ok()) return directory_status;
  }
  for (std::filesystem::recursive_directory_iterator iterator(objects, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (iterator->is_symlink(error)) {
      return Status{StatusCode::kCorruption, "persisted tiered path contains a symbolic link"};
    }
    if (!iterator->is_regular_file(error) || iterator->path().filename() != "object.meta") continue;
    const auto file_size = iterator->file_size(error);
    if (error || file_size > impl->config.max_metadata_bytes) {
      return Status{StatusCode::kCorruption, "invalid persisted metadata size"};
    }
    auto metadata = ReadFile(iterator->path(), file_size, impl->config.max_metadata_bytes,
                             impl->config.io_fault, "metadata");
    if (!metadata.ok()) return metadata.status();
    auto record = DecodeMetadata(metadata.value(), impl->config);
    if (!record.ok()) return record.status();
    const std::string hex = iterator->path().parent_path().filename().string();
    const auto object_directory = iterator->path().parent_path();
    const auto shard_directory = object_directory.parent_path();
    if (hex.size() != kDigestBytes * 2U || shard_directory.parent_path() != objects ||
        shard_directory.filename() != hex.substr(0, 2) ||
        !RequireRealDirectory(shard_directory).ok() ||
        !RequireRealDirectory(object_directory).ok()) {
      return Status{StatusCode::kCorruption, "invalid persisted object directory"};
    }
    auto canonical_digest = Sha256(record.value().canonical);
    if (!canonical_digest.ok() || DigestHex(canonical_digest.value()) != hex) {
      return Status{StatusCode::kCorruption, "persisted object key mismatch"};
    }
    CacheKey key{kManifestVersion, canonical_digest.value()};
    std::uint64_t disk_bytes = static_cast<std::uint64_t>(file_size);
    for (const auto& chunk : record.value().chunks) {
      const auto chunk_path = iterator->path().parent_path() / DigestHex(chunk.sha);
      const auto size = std::filesystem::file_size(chunk_path, error);
      if (error || size != chunk.size ||
          chunk.size > impl->config.disk_budget_bytes -
                           std::min(impl->config.disk_budget_bytes, disk_bytes)) {
        return Status{StatusCode::kCorruption, "persisted disk chunk is missing or invalid"};
      }
      disk_bytes += chunk.size;
    }
    if (disk_bytes > impl->config.disk_budget_bytes -
                         std::min(impl->config.disk_budget_bytes, impl->disk_used)) {
      return Status{StatusCode::kLimitExceeded, "persisted objects exceed disk budget"};
    }
    record.value().disk_bytes = disk_bytes;
    auto entry = std::make_shared<Impl::Entry>();
    entry->key = key;
    entry->canonical = record.value().canonical;
    entry->payload_digest = record.value().payload_digest;
    entry->disk = std::move(record.value());
    if (!impl->entries.emplace(key.ToString(), std::move(entry)).second) {
      return Status{StatusCode::kCorruption, "duplicate persisted canonical object"};
    }
    impl->disk_used += disk_bytes;
  }
  if (error) return Status{StatusCode::kIoError, "cannot scan tiered store directory"};
  for (std::filesystem::recursive_directory_iterator iterator(objects, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (iterator->is_symlink(error)) {
      return Status{StatusCode::kCorruption, "temporary tiered path contains a symbolic link"};
    }
    if (iterator->is_regular_file(error) && iterator->path().extension() == ".tmp") {
      std::filesystem::remove(iterator->path(), error);
    }
  }
  if (error) return Status{StatusCode::kIoError, "cannot clean temporary tiered store files"};
  for (std::filesystem::directory_iterator shard(objects, error), end; !error && shard != end;
       shard.increment(error)) {
    if (shard->is_symlink(error)) {
      return Status{StatusCode::kCorruption, "tiered shard is a symbolic link"};
    }
    if (!shard->is_directory(error)) continue;
    for (std::filesystem::directory_iterator object(shard->path(), error), object_end;
         !error && object != object_end; object.increment(error)) {
      if (object->is_symlink(error)) {
        return Status{StatusCode::kCorruption, "tiered object path is a symbolic link"};
      }
      if (object->is_directory(error) &&
          !std::filesystem::exists(object->path() / "object.meta", error)) {
        std::filesystem::remove_all(object->path(), error);
      }
    }
  }
  if (error) return Status{StatusCode::kIoError, "cannot clean unpublished tiered objects"};
  return std::unique_ptr<TieredStore>(new TieredStore(std::move(impl)));
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "unable to open tiered store"};
} catch (const std::filesystem::filesystem_error&) {
  return Status{StatusCode::kIoError, "cannot open tiered store"};
} catch (...) {
  return Status{StatusCode::kInternal, "tiered store open failed"};
}

Result<TieredResidentHandle> TieredStore::Put(const TensorManifest& manifest,
                                              const std::vector<ByteView>& chunks) try {
  const Status validation = ValidateManifest(manifest);
  if (!validation.ok()) return validation;
  if (manifest.payload_bytes > impl_->config.max_object_bytes ||
      manifest.chunk_count > impl_->config.max_chunks_per_object ||
      chunks.size() != manifest.chunk_count) {
    return Status{StatusCode::kLimitExceeded, "tiered object exceeds configured limits"};
  }
  auto key = CanonicalCacheKey(manifest);
  auto canonical = EncodeCanonicalManifest(manifest);
  if (!key.ok()) return key.status();
  if (!canonical.ok()) return canonical.status();
  DiskRecord disk;
  disk.canonical = canonical.value();
  disk.payload_digest = manifest.payload_digest;
  disk.payload_bytes = manifest.payload_bytes;
  disk.created_at_ns = manifest.created_at_ns;
  disk.accessed_at_ns = manifest.accessed_at_ns;
  std::vector<ResidentAllocation> allocations;
  allocations.reserve(chunks.size());
  disk.chunks.reserve(chunks.size());
  std::uint64_t total = 0;
  for (std::size_t index = 0; index < chunks.size(); ++index) {
    const std::uint64_t expected = std::min<std::uint64_t>(
        manifest.chunk_bytes,
        manifest.payload_bytes - static_cast<std::uint64_t>(index) * manifest.chunk_bytes);
    if (chunks[index].size() != expected) {
      return Status{StatusCode::kInvalidArgument, "chunk size does not match manifest"};
    }
    auto digest = Sha256(chunks[index]);
    if (!digest.ok()) return digest.status();
    disk.chunks.push_back({expected, Crc32(chunks[index]), digest.value()});
    auto allocation = impl_->pool.Copy(chunks[index]);
    if (!allocation.ok()) return allocation.status();
    allocations.push_back(std::move(allocation.value()));
    total += expected;
  }
  auto payload_digest = Sha256Parts(chunks);
  if (!payload_digest.ok()) return payload_digest.status();
  if (payload_digest.value() != manifest.payload_digest) {
    return Status{StatusCode::kCorruption, "payload SHA-256 mismatch"};
  }
  auto metadata = EncodeMetadata(disk);
  if (!metadata.ok() || metadata.value().size() > impl_->config.max_metadata_bytes) {
    return metadata.ok() ? Status{StatusCode::kLimitExceeded, "disk metadata limit exceeded"}
                         : metadata.status();
  }
  disk.disk_bytes = total + metadata.value().size();
  auto pin = std::make_shared<TieredResidentHandle::Pin>(
      TieredResidentHandle::Pin{manifest, key.value(), std::move(allocations), nullptr});
  const std::string key_string = key.value().ToString();
  const auto directory = ObjectPath(impl_->config.directory, key.value());
  const auto objects_root = impl_->config.directory / "objects";
  auto entry = std::make_shared<Impl::Entry>();
  entry->state = TierState::kLoading;
  entry->key = key.value();
  entry->canonical = canonical.value();
  entry->payload_digest = manifest.payload_digest;
  entry->disk = disk;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->entries.contains(key_string)) {
      return Status{StatusCode::kAlreadyExists, "tiered object already exists"};
    }
    const std::uint64_t charged = impl_->disk_used + impl_->disk_reserved;
    if (charged < impl_->disk_used ||
        disk.disk_bytes >
            impl_->config.disk_budget_bytes - std::min(impl_->config.disk_budget_bytes, charged)) {
      return Status{StatusCode::kLimitExceeded, "tiered disk quota exceeded"};
    }
    impl_->entries.emplace(key_string, entry);
    impl_->disk_reserved += disk.disk_bytes;
  }
  std::error_code error;
  Status io_status;
  bool metadata_published = false;
  try {
    std::filesystem::create_directories(directory, error);
    io_status =
        error ? Status{StatusCode::kIoError, "cannot create object directory"} : Status::Ok();
    if (io_status.ok()) io_status = RequireRealDirectory(objects_root);
    if (io_status.ok()) io_status = RequireRealDirectory(directory.parent_path());
    if (io_status.ok()) io_status = RequireRealDirectory(directory);
    for (std::size_t index = 0; io_status.ok() && index < chunks.size(); ++index) {
      const auto final = directory / DigestHex(disk.chunks[index].sha);
      io_status = PublishFile(final.string() + ".tmp", final, chunks[index], impl_->config.io_fault,
                              "chunk");
    }
    if (io_status.ok()) {
      const auto final = directory / "object.meta";
      io_status = PublishFile(final.string() + ".tmp", final, metadata.value(),
                              impl_->config.io_fault, "metadata", &metadata_published);
    }
    if (io_status.ok())
      io_status = SyncDirectory(directory.parent_path(), impl_->config.io_fault, "shard.fsync");
    if (io_status.ok())
      io_status = SyncDirectory(objects_root, impl_->config.io_fault, "objects.fsync");
  } catch (const std::bad_alloc&) {
    io_status = {StatusCode::kLimitExceeded, "unable to allocate publication path"};
  } catch (const std::filesystem::filesystem_error&) {
    io_status = {StatusCode::kIoError, "tiered object publication path failed"};
  } catch (...) {
    io_status = {StatusCode::kInternal, "tiered object publication failed"};
  }
  if (!io_status.ok()) {
    if (!metadata_published) std::filesystem::remove_all(directory, error);
    std::lock_guard lock(impl_->mutex);
    impl_->disk_reserved -= disk.disk_bytes;
    entry->state = TierState::kFailed;
    entry->last_error = io_status;
    entry->changed.notify_all();
    if (metadata_published) {
      impl_->disk_used += entry->disk.disk_bytes;
    } else {
      impl_->entries.erase(key_string);
    }
    return io_status;
  }
  {
    std::lock_guard lock(impl_->mutex);
    entry->resident = pin;
    entry->state = TierState::kResident;
    impl_->disk_reserved -= entry->disk.disk_bytes;
    impl_->disk_used += entry->disk.disk_bytes;
    entry->changed.notify_all();
  }
  return TieredResidentHandle(std::move(pin));
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "unable to put tiered object"};
} catch (const std::filesystem::filesystem_error&) {
  return Status{StatusCode::kIoError, "tiered object publication failed"};
} catch (...) {
  return Status{StatusCode::kInternal, "tiered object publication failed"};
}

Result<TieredResidentHandle> TieredStore::Lookup(const TensorManifest& expected, Deadline deadline,
                                                 std::stop_token stop_token) try {
  const Status validation = ValidateManifestIdentity(expected);
  if (!validation.ok()) return validation;
  auto key = CanonicalCacheKey(expected);
  auto canonical = EncodeCanonicalManifest(expected);
  if (!key.ok()) return key.status();
  if (!canonical.ok()) return canonical.status();
  std::shared_ptr<Impl::Entry> entry;
  {
    std::unique_lock lock(impl_->mutex);
    const auto iterator = impl_->entries.find(key.value().ToString());
    if (iterator == impl_->entries.end()) {
      return Status{StatusCode::kNotFound, "tiered object does not exist"};
    }
    entry = iterator->second;
    if (entry->canonical != canonical.value()) {
      return Status{StatusCode::kCorruption, "cache key resolved to incompatible metadata"};
    }
    while (entry->state == TierState::kLoading || entry->state == TierState::kEvicting) {
      if (stop_token.stop_requested()) {
        return Status{StatusCode::kCancelled, "tiered lookup cancelled"};
      }
      if (deadline != Deadline::max() && std::chrono::steady_clock::now() >= deadline) {
        return Status{StatusCode::kCancelled, "tiered lookup deadline exceeded"};
      }
      const bool ready = deadline == Deadline::max()
                             ? entry->changed.wait(lock, stop_token,
                                                   [&] {
                                                     return entry->state != TierState::kLoading &&
                                                            entry->state != TierState::kEvicting;
                                                   })
                             : entry->changed.wait_until(lock, stop_token, deadline, [&] {
                                 return entry->state != TierState::kLoading &&
                                        entry->state != TierState::kEvicting;
                               });
      if (!ready) return Status{StatusCode::kCancelled, "tiered lookup wait ended"};
    }
    if (entry->state == TierState::kResident) {
      return TieredResidentHandle(entry->resident);
    }
    if (entry->state == TierState::kFailed) return entry->last_error;
    if (stop_token.stop_requested() ||
        (deadline != Deadline::max() && std::chrono::steady_clock::now() >= deadline)) {
      return Status{StatusCode::kCancelled, "tiered lookup cancelled before load"};
    }
    entry->state = TierState::kLoading;
  }
  auto loaded = impl_->Load(entry, expected);
  std::lock_guard lock(impl_->mutex);
  if (!loaded.ok()) {
    entry->last_error = loaded.status();
    entry->state = (loaded.status().code() == StatusCode::kLimitExceeded ||
                    loaded.status().code() == StatusCode::kIoError)
                       ? TierState::kDiskOnly
                       : TierState::kFailed;
    entry->changed.notify_all();
    return loaded.status();
  }
  entry->resident = loaded.value();
  entry->state = TierState::kResident;
  entry->changed.notify_all();
  if (stop_token.stop_requested() ||
      (deadline != Deadline::max() && std::chrono::steady_clock::now() >= deadline)) {
    return Status{StatusCode::kCancelled, "tiered lookup completed after waiter cancellation"};
  }
  return TieredResidentHandle(entry->resident);
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "unable to lookup tiered object"};
} catch (...) {
  return Status{StatusCode::kInternal, "tiered object lookup failed"};
}

Result<TieredResidentHandle> TieredStore::LoadRanges(const TensorManifest& source,
                                                     std::span<const TensorRange> ranges,
                                                     Deadline deadline,
                                                     std::stop_token stop_token) try {
  const Status validation = ValidateManifestIdentity(source);
  if (!validation.ok()) return validation;
  if (ranges.empty() || stop_token.stop_requested() ||
      (deadline != Deadline::max() && std::chrono::steady_clock::now() >= deadline)) {
    return Status{StatusCode::kCancelled, "tiered range load cancelled"};
  }
  auto key = CanonicalCacheKey(source);
  auto canonical = EncodeCanonicalManifest(source);
  if (!key.ok()) return key.status();
  if (!canonical.ok()) return canonical.status();
  std::shared_ptr<Impl::Entry> entry;
  {
    std::lock_guard lock(impl_->mutex);
    const auto iterator = impl_->entries.find(key.value().ToString());
    if (iterator == impl_->entries.end()) return Status{StatusCode::kNotFound, "tiered object does not exist"};
    entry = iterator->second;
    if (entry->canonical != canonical.value()) return Status{StatusCode::kCorruption, "metadata mismatch"};
    if (entry->state == TierState::kLoading || entry->state == TierState::kEvicting)
      return Status{StatusCode::kBusy, "tiered object is changing"};
    if (entry->state == TierState::kFailed) return entry->last_error;
  }
  auto loaded = impl_->LoadRanges(entry, source, ranges);
  if (!loaded.ok()) return loaded.status();
  if (stop_token.stop_requested() ||
      (deadline != Deadline::max() && std::chrono::steady_clock::now() >= deadline))
    return Status{StatusCode::kCancelled, "tiered range load completed after cancellation"};
  return TieredResidentHandle(loaded.value());
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "unable to load tiered ranges"};
} catch (...) {
  return Status{StatusCode::kInternal, "tiered range load failed"};
}

Status TieredStore::Evict(const TensorManifest& expected) try {
  auto key = CanonicalCacheKey(expected);
  auto canonical = EncodeCanonicalManifest(expected);
  if (!key.ok()) return key.status();
  if (!canonical.ok()) return canonical.status();
  std::lock_guard lock(impl_->mutex);
  const auto iterator = impl_->entries.find(key.value().ToString());
  if (iterator == impl_->entries.end()) return {StatusCode::kNotFound, "tiered object not found"};
  auto& entry = *iterator->second;
  if (entry.canonical != canonical.value()) return {StatusCode::kCorruption, "metadata mismatch"};
  if (entry.state != TierState::kResident) return {StatusCode::kBusy, "object is not resident"};
  entry.state = TierState::kEvicting;
  entry.resident.reset();
  entry.state = TierState::kDiskOnly;
  entry.changed.notify_all();
  return Status::Ok();
} catch (const std::bad_alloc&) {
  return {StatusCode::kLimitExceeded, "unable to evict tiered object"};
} catch (...) {
  return {StatusCode::kInternal, "tiered object eviction failed"};
}

Status TieredStore::Delete(const TensorManifest& expected) {
  std::shared_ptr<Impl::Entry> entry;
  try {
    auto key = CanonicalCacheKey(expected);
    auto canonical = EncodeCanonicalManifest(expected);
    if (!key.ok()) return key.status();
    if (!canonical.ok()) return canonical.status();
    const std::string key_string = key.value().ToString();
    const auto directory = ObjectPath(impl_->config.directory, key.value());
    const auto shard = directory.parent_path();
    const auto objects_root = shard.parent_path();
    {
      std::lock_guard lock(impl_->mutex);
      const auto iterator = impl_->entries.find(key_string);
      if (iterator == impl_->entries.end())
        return {StatusCode::kNotFound, "tiered object not found"};
      entry = iterator->second;
      if (entry->canonical != canonical.value())
        return {StatusCode::kCorruption, "metadata mismatch"};
      if (entry->state == TierState::kLoading || entry->state == TierState::kEvicting) {
        return {StatusCode::kBusy, "tiered object migration is active"};
      }
      entry->last_error = {StatusCode::kInternal, "tiered object deletion did not complete"};
      entry->state = TierState::kEvicting;
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    Status deletion_status =
        error ? Status{StatusCode::kIoError, "tiered object removal failed"} : Status::Ok();
    if (deletion_status.ok())
      deletion_status = SyncDirectory(shard, impl_->config.io_fault, "delete.shard.fsync");
    if (deletion_status.ok())
      deletion_status = SyncDirectory(objects_root, impl_->config.io_fault, "delete.objects.fsync");
    std::lock_guard lock(impl_->mutex);
    if (!deletion_status.ok()) {
      entry->state = TierState::kFailed;
      entry->last_error = deletion_status;
      entry->changed.notify_all();
      if (!error) {
        impl_->entries.erase(key_string);
        impl_->disk_used -= entry->disk.disk_bytes;
      }
      return deletion_status;
    }
    impl_->entries.erase(key_string);
    impl_->disk_used -= entry->disk.disk_bytes;
    entry->last_error = {StatusCode::kNotFound, "tiered object was deleted"};
    entry->state = TierState::kFailed;
    entry->changed.notify_all();
    return Status::Ok();
  } catch (const std::bad_alloc&) {
    if (entry != nullptr) {
      std::lock_guard lock(impl_->mutex);
      entry->state = TierState::kFailed;
      entry->changed.notify_all();
    }
    return {StatusCode::kLimitExceeded, "unable to delete tiered object"};
  } catch (const std::filesystem::filesystem_error&) {
    if (entry != nullptr) {
      std::lock_guard lock(impl_->mutex);
      entry->state = TierState::kFailed;
      entry->changed.notify_all();
    }
    return {StatusCode::kIoError, "tiered object disk reclamation failed"};
  } catch (...) {
    if (entry != nullptr) {
      std::lock_guard lock(impl_->mutex);
      entry->state = TierState::kFailed;
      entry->changed.notify_all();
    }
    return {StatusCode::kInternal, "tiered object deletion failed"};
  }
}

Result<TierState> TieredStore::State(const TensorManifest& expected) const try {
  auto key = CanonicalCacheKey(expected);
  auto canonical = EncodeCanonicalManifest(expected);
  if (!key.ok()) return key.status();
  if (!canonical.ok()) return canonical.status();
  std::lock_guard lock(impl_->mutex);
  const auto iterator = impl_->entries.find(key.value().ToString());
  if (iterator == impl_->entries.end())
    return Status{StatusCode::kNotFound, "tiered object not found"};
  if (iterator->second->canonical != canonical.value()) {
    return Status{StatusCode::kCorruption, "metadata mismatch"};
  }
  return iterator->second->state;
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "unable to inspect tiered object"};
} catch (...) {
  return Status{StatusCode::kInternal, "tiered state inspection failed"};
}

TieredStoreStats TieredStore::Stats() const {
  std::lock_guard lock(impl_->mutex);
  return {impl_->pool.Stats(), impl_->disk_used, impl_->config.disk_budget_bytes,
          impl_->disk_reads.load(std::memory_order_relaxed),
          static_cast<std::uint64_t>(impl_->entries.size())};
}

Result<std::vector<TensorManifest>> TieredStore::ListManifests() const {
  std::lock_guard lock(impl_->mutex);
  std::vector<TensorManifest> manifests;
  manifests.reserve(impl_->entries.size());
  for (const auto& [key, entry] : impl_->entries) {
    (void)key;
    auto manifest = DecodeCanonicalManifest(entry->canonical);
    if (!manifest.ok()) return manifest.status();
    manifest.value().payload_digest = entry->payload_digest;
    manifests.push_back(std::move(manifest.value()));
  }
  return manifests;
}

}  // namespace kvstore::kvcache
