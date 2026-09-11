#include "kvstore/kvcache/chunk_registry.hpp"

#include <picosha2.h>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

#include "kvstore/common/crc32.hpp"

namespace kvstore::kvcache {

namespace {

constexpr std::uint64_t kObjectAndIndexOverheadBytes = 1024;
constexpr std::uint64_t kChunkAllocatorOverheadBytes = 64;

Result<std::uint64_t> AccountedBytes(const TensorManifest& manifest, std::size_t canonical_bytes) {
  constexpr std::uint64_t kChunkOverhead = sizeof(ResidentChunk) +
                                           sizeof(std::shared_ptr<const ResidentChunk>) +
                                           kChunkAllocatorOverheadBytes;
  const std::uint64_t chunk_overhead =
      static_cast<std::uint64_t>(manifest.chunk_count) * kChunkOverhead;
  const std::uint64_t canonical = static_cast<std::uint64_t>(canonical_bytes);
  if (canonical > (std::numeric_limits<std::uint64_t>::max() - kObjectAndIndexOverheadBytes) / 2U ||
      chunk_overhead > std::numeric_limits<std::uint64_t>::max() - kObjectAndIndexOverheadBytes -
                           (canonical * 2U) ||
      manifest.payload_bytes > std::numeric_limits<std::uint64_t>::max() -
                                   kObjectAndIndexOverheadBytes - (canonical * 2U) -
                                   chunk_overhead) {
    return Status{StatusCode::kLimitExceeded, "tracked object size overflows"};
  }
  return kObjectAndIndexOverheadBytes + (canonical * 2U) + chunk_overhead + manifest.payload_bytes;
}

Result<Bytes> Canonical(const TensorManifest& manifest) {
  return EncodeCanonicalManifest(manifest);
}

Result<Digest> PayloadDigest(const std::vector<std::shared_ptr<const ResidentChunk>>& chunks) try {
  picosha2::hash256_one_by_one hasher;
  std::array<unsigned char, 4096> input{};
  for (const auto& chunk : chunks) {
    for (std::size_t offset = 0; offset < chunk->bytes.size();) {
      const std::size_t size = std::min(input.size(), chunk->bytes.size() - offset);
      for (std::size_t index = 0; index < size; ++index) {
        input[index] = std::to_integer<unsigned char>(chunk->bytes[offset + index]);
      }
      hasher.process(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(size));
      offset += size;
    }
  }
  hasher.finish();
  std::array<unsigned char, kDigestBytes> output{};
  hasher.get_hash_bytes(output.begin(), output.end());
  Digest digest{};
  for (std::size_t index = 0; index < digest.size(); ++index) {
    digest[index] = static_cast<std::byte>(output[index]);
  }
  return digest;
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "unable to allocate payload digest state"};
}

}  // namespace

const TensorManifest& ResidentHandle::manifest() const { return object_->manifest; }

const CacheKey& ResidentHandle::key() const { return object_->key; }

std::size_t ResidentHandle::chunk_count() const { return object_->chunks.size(); }

Result<ByteView> ResidentHandle::Chunk(std::size_t index) const {
  if (index >= object_->chunks.size()) {
    return Status{StatusCode::kInvalidArgument, "chunk index is out of range"};
  }
  return ByteView{object_->chunks[index]->bytes};
}

Result<std::uint32_t> ResidentHandle::ChunkChecksum(std::size_t index) const {
  if (index >= object_->chunks.size()) {
    return Status{StatusCode::kInvalidArgument, "chunk index is out of range"};
  }
  return object_->chunks[index]->checksum;
}

ResidentHandle::Object::~Object() {
  if (live_bytes != nullptr && accounted_bytes != 0) {
    live_bytes->fetch_sub(accounted_bytes, std::memory_order_relaxed);
  }
}

ChunkRegistry::ChunkRegistry(RegistryLimits limits, KeyFunction key_function)
    : limits_(limits),
      key_function_(key_function ? std::move(key_function) : KeyFunction{CanonicalCacheKey}),
      live_bytes_(std::make_shared<std::atomic<std::uint64_t>>(0)) {}

Result<std::uint64_t> ChunkRegistry::Reserve(const TensorManifest& manifest) try {
  const Status manifest_status = ValidateManifest(manifest);
  if (!manifest_status.ok()) {
    return manifest_status;
  }
  auto canonical = Canonical(manifest);
  if (!canonical.ok()) {
    return canonical.status();
  }
  auto key = key_function_(manifest);
  if (!key.ok()) {
    return key.status();
  }
  if (key.value().version != manifest.version) {
    return Status{StatusCode::kInvalidArgument, "cache key version does not match manifest"};
  }
  const std::string key_string = key.value().ToString();
  const auto accounted = AccountedBytes(manifest, canonical.value().size());
  if (!accounted.ok()) {
    return accounted.status();
  }

  std::lock_guard lock(mutex_);
  if (limits_.max_objects == 0 || limits_.max_pending_reservations == 0 ||
      limits_.max_chunks_per_object == 0 || limits_.max_object_bytes == 0 ||
      limits_.max_pending_bytes == 0 || limits_.max_tracked_bytes == 0) {
    return Status{StatusCode::kInvalidArgument, "chunk registry limits must be positive"};
  }
  const auto object = objects_.find(key_string);
  if (object != objects_.end()) {
    if (object->second->canonical_manifest != canonical.value()) {
      return Status{StatusCode::kCorruption, "canonical cache key collision"};
    }
    if (object->second->manifest.payload_digest != manifest.payload_digest) {
      return Status{StatusCode::kCorruption, "cache identity has conflicting payload digest"};
    }
    return Status{StatusCode::kAlreadyExists, "cache object already exists"};
  }
  const auto pending_key = reservation_keys_.find(key_string);
  if (pending_key != reservation_keys_.end()) {
    const auto pending = reservations_.find(pending_key->second);
    if (pending != reservations_.end() &&
        pending->second.manifest.payload_digest != manifest.payload_digest) {
      return Status{StatusCode::kCorruption, "cache identity has conflicting payload digest"};
    }
    return Status{StatusCode::kBusy, "cache object publication is already in progress"};
  }
  if (objects_.size() >= limits_.max_objects ||
      reservations_.size() >= limits_.max_pending_reservations ||
      manifest.chunk_count > limits_.max_chunks_per_object ||
      manifest.payload_bytes > limits_.max_object_bytes ||
      accounted.value() >
          limits_.max_pending_bytes - std::min(limits_.max_pending_bytes, pending_bytes_)) {
    return Status{StatusCode::kLimitExceeded, "chunk registry object or pending limit exceeded"};
  }
  const std::uint64_t live_bytes = live_bytes_->load(std::memory_order_relaxed);
  if (live_bytes > limits_.max_tracked_bytes ||
      pending_bytes_ > limits_.max_tracked_bytes - live_bytes ||
      accounted.value() > limits_.max_tracked_bytes - live_bytes - pending_bytes_) {
    return Status{StatusCode::kLimitExceeded, "chunk registry byte budget exceeded"};
  }
  if (next_reservation_id_ == std::numeric_limits<std::uint64_t>::max()) {
    return Status{StatusCode::kLimitExceeded, "reservation id space exhausted"};
  }
  const std::uint64_t id = next_reservation_id_;
  try {
    Reservation reservation{manifest, key.value(), std::move(canonical.value()),
                            {},       0,           accounted.value()};
    reservation.chunks.resize(reservation.manifest.chunk_count);
    reservations_.emplace(id, std::move(reservation));
    try {
      reservation_keys_.emplace(key_string, id);
    } catch (const std::bad_alloc&) {
      reservations_.erase(id);
      return Status{StatusCode::kLimitExceeded, "unable to allocate reservation index"};
    }
  } catch (const std::bad_alloc&) {
    return Status{StatusCode::kLimitExceeded, "unable to allocate chunk reservation"};
  }
  ++next_reservation_id_;
  pending_bytes_ += reservations_.at(id).accounted_bytes;
  return id;
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "unable to allocate cache reservation"};
} catch (...) {
  return Status{StatusCode::kInternal, "cache key function failed"};
}

Status ChunkRegistry::Put(std::uint64_t reservation_id, std::uint32_t chunk_index, ByteView bytes,
                          std::uint32_t expected_checksum) {
  std::lock_guard lock(mutex_);
  const auto iterator = reservations_.find(reservation_id);
  if (iterator == reservations_.end()) {
    return {StatusCode::kNotFound, "reservation does not exist"};
  }
  Reservation& reservation = iterator->second;
  if (chunk_index >= reservation.manifest.chunk_count) {
    return {StatusCode::kInvalidArgument, "chunk index is out of range"};
  }
  const std::uint64_t offset = static_cast<std::uint64_t>(chunk_index) *
                               static_cast<std::uint64_t>(reservation.manifest.chunk_bytes);
  const std::uint64_t expected_size = std::min<std::uint64_t>(
      reservation.manifest.chunk_bytes, reservation.manifest.payload_bytes - offset);
  if (bytes.size() != expected_size) {
    return {StatusCode::kInvalidArgument, "chunk size does not match manifest"};
  }
  if (reservation.chunks[chunk_index] != nullptr) {
    return {StatusCode::kAlreadyExists, "chunk already uploaded"};
  }
  if (Crc32(bytes) != expected_checksum) {
    return {StatusCode::kCorruption, "chunk checksum mismatch"};
  }
  std::shared_ptr<ResidentChunk> chunk;
  try {
    chunk = std::make_shared<ResidentChunk>(
        ResidentChunk{chunk_index, expected_checksum, Bytes(bytes.begin(), bytes.end())});
  } catch (const std::bad_alloc&) {
    return {StatusCode::kLimitExceeded, "unable to allocate resident chunk"};
  }
  reservation.received_bytes += static_cast<std::uint64_t>(bytes.size());
  reservation.chunks[chunk_index] = std::move(chunk);
  return Status::Ok();
}

Result<ResidentHandle> ChunkRegistry::Commit(std::uint64_t reservation_id) try {
  std::lock_guard lock(mutex_);
  const auto iterator = reservations_.find(reservation_id);
  if (iterator == reservations_.end()) {
    return Status{StatusCode::kNotFound, "reservation does not exist"};
  }
  Reservation& reservation = iterator->second;
  if (reservation.received_bytes != reservation.manifest.payload_bytes ||
      std::ranges::any_of(reservation.chunks, [](const auto& chunk) { return chunk == nullptr; })) {
    return Status{StatusCode::kBusy, "cache object is incomplete"};
  }
  const auto payload_digest = PayloadDigest(reservation.chunks);
  if (!payload_digest.ok()) {
    return payload_digest.status();
  }
  if (payload_digest.value() != reservation.manifest.payload_digest) {
    return Status{StatusCode::kCorruption, "payload checksum mismatch"};
  }

  const std::string key_string = reservation.key.ToString();
  if (objects_.size() >= limits_.max_objects) {
    return Status{StatusCode::kLimitExceeded, "published object limit exceeded"};
  }
  std::shared_ptr<ResidentHandle::Object> object;
  try {
    object = std::make_shared<ResidentHandle::Object>(
        ResidentHandle::Object{reservation.manifest, reservation.key,
                               reservation.canonical_manifest, reservation.chunks, live_bytes_, 0});
  } catch (const std::bad_alloc&) {
    return Status{StatusCode::kLimitExceeded, "unable to allocate resident object"};
  }
  live_bytes_->fetch_add(reservation.accounted_bytes, std::memory_order_relaxed);
  object->accounted_bytes = reservation.accounted_bytes;
  std::unordered_map<std::string, std::shared_ptr<const ResidentHandle::Object>>::iterator
      published;
  bool inserted = false;
  try {
    const auto result = objects_.emplace(key_string, object);
    published = result.first;
    inserted = result.second;
  } catch (const std::bad_alloc&) {
    return Status{StatusCode::kLimitExceeded, "unable to publish resident object"};
  }
  if (!inserted) {
    if (published->second->canonical_manifest != object->canonical_manifest) {
      return Status{StatusCode::kCorruption, "canonical cache key collision"};
    }
    return Status{StatusCode::kAlreadyExists, "cache object already exists"};
  }
  pending_bytes_ -= reservation.accounted_bytes;
  reservation_keys_.erase(key_string);
  reservations_.erase(iterator);
  return ResidentHandle(std::move(object));
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "unable to publish resident object"};
} catch (...) {
  return Status{StatusCode::kInternal, "cache publication failed"};
}

Status ChunkRegistry::Abort(std::uint64_t reservation_id) try {
  std::lock_guard lock(mutex_);
  const auto iterator = reservations_.find(reservation_id);
  if (iterator == reservations_.end()) {
    return {StatusCode::kNotFound, "reservation does not exist"};
  }
  pending_bytes_ -= iterator->second.accounted_bytes;
  reservation_keys_.erase(iterator->second.key.ToString());
  reservations_.erase(iterator);
  return Status::Ok();
} catch (const std::bad_alloc&) {
  return {StatusCode::kLimitExceeded, "unable to release reservation index"};
} catch (...) {
  return {StatusCode::kInternal, "cache reservation release failed"};
}

Result<ResidentHandle> ChunkRegistry::Lookup(const TensorManifest& expected) const try {
  auto canonical = Canonical(expected);
  if (!canonical.ok()) {
    return canonical.status();
  }
  auto key = key_function_(expected);
  if (!key.ok()) {
    return key.status();
  }
  std::lock_guard lock(mutex_);
  const auto iterator = objects_.find(key.value().ToString());
  if (iterator == objects_.end()) {
    return Status{StatusCode::kNotFound, "cache object does not exist"};
  }
  if (iterator->second->canonical_manifest != canonical.value()) {
    return Status{StatusCode::kCorruption, "cache key resolved to incompatible tensor metadata"};
  }
  return ResidentHandle(iterator->second);
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "unable to allocate cache lookup key"};
} catch (...) {
  return Status{StatusCode::kInternal, "cache lookup key function failed"};
}

Status ChunkRegistry::Delete(const TensorManifest& expected) try {
  auto canonical = Canonical(expected);
  if (!canonical.ok()) {
    return canonical.status();
  }
  auto key = key_function_(expected);
  if (!key.ok()) {
    return key.status();
  }
  std::lock_guard lock(mutex_);
  const auto iterator = objects_.find(key.value().ToString());
  if (iterator == objects_.end()) {
    return {StatusCode::kNotFound, "cache object does not exist"};
  }
  if (iterator->second->canonical_manifest != canonical.value()) {
    return {StatusCode::kCorruption, "cache key resolved to incompatible tensor metadata"};
  }
  objects_.erase(iterator);
  return Status::Ok();
} catch (const std::bad_alloc&) {
  return {StatusCode::kLimitExceeded, "unable to allocate cache deletion key"};
} catch (...) {
  return {StatusCode::kInternal, "cache deletion key function failed"};
}

std::size_t ChunkRegistry::Size() const {
  std::lock_guard lock(mutex_);
  return objects_.size();
}

std::size_t ChunkRegistry::Pending() const {
  std::lock_guard lock(mutex_);
  return reservations_.size();
}

std::uint64_t ChunkRegistry::TrackedBytes() const {
  std::lock_guard lock(mutex_);
  return live_bytes_->load(std::memory_order_relaxed) + pending_bytes_;
}

}  // namespace kvstore::kvcache
