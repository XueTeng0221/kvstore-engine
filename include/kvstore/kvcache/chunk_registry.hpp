#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "kvstore/kvcache/model.hpp"

namespace kvstore::kvcache {

struct ResidentChunk {
  std::uint32_t index{};
  std::uint32_t checksum{};
  Bytes bytes;
};

struct RegistryLimits {
  std::size_t max_objects{1'000'000};
  std::size_t max_pending_reservations{64};
  std::uint32_t max_chunks_per_object{4096};
  std::uint64_t max_object_bytes{256U * 1024U * 1024U};
  std::uint64_t max_pending_bytes{512U * 1024U * 1024U};
  std::uint64_t max_tracked_bytes{512U * 1024U * 1024U};
};

class ResidentHandle {
 public:
  [[nodiscard]] const TensorManifest& manifest() const;
  [[nodiscard]] const CacheKey& key() const;
  [[nodiscard]] std::size_t chunk_count() const;
  [[nodiscard]] Result<ByteView> Chunk(std::size_t index) const;
  [[nodiscard]] Result<std::uint32_t> ChunkChecksum(std::size_t index) const;

 private:
  struct Object {
    TensorManifest manifest;
    CacheKey key;
    Bytes canonical_manifest;
    std::vector<std::shared_ptr<const ResidentChunk>> chunks;
    std::shared_ptr<std::atomic<std::uint64_t>> live_bytes;
    std::uint64_t accounted_bytes{};

    ~Object();
  };
  explicit ResidentHandle(std::shared_ptr<const Object> object) : object_(std::move(object)) {}

  std::shared_ptr<const Object> object_;

  friend class ChunkRegistry;
};

class ChunkRegistry {
 public:
  using KeyFunction = std::function<Result<CacheKey>(const TensorManifest&)>;

  explicit ChunkRegistry(RegistryLimits limits = {}, KeyFunction key_function = CanonicalCacheKey);

  [[nodiscard]] Result<std::uint64_t> Reserve(const TensorManifest& manifest);
  [[nodiscard]] Status Put(std::uint64_t reservation_id, std::uint32_t chunk_index, ByteView bytes,
                           std::uint32_t expected_checksum);
  [[nodiscard]] Result<ResidentHandle> Commit(std::uint64_t reservation_id);
  [[nodiscard]] Status Abort(std::uint64_t reservation_id);
  [[nodiscard]] Result<ResidentHandle> Lookup(const TensorManifest& expected) const;
  [[nodiscard]] Status Delete(const TensorManifest& expected);
  [[nodiscard]] std::size_t Size() const;
  [[nodiscard]] std::size_t Pending() const;
  [[nodiscard]] std::uint64_t TrackedBytes() const;

 private:
  struct Reservation {
    TensorManifest manifest;
    CacheKey key;
    Bytes canonical_manifest;
    std::vector<std::shared_ptr<const ResidentChunk>> chunks;
    std::uint64_t received_bytes{};
    std::uint64_t accounted_bytes{};
  };

  RegistryLimits limits_;
  KeyFunction key_function_;
  std::shared_ptr<std::atomic<std::uint64_t>> live_bytes_;
  mutable std::mutex mutex_;
  std::uint64_t next_reservation_id_{1};
  std::uint64_t pending_bytes_{};
  std::unordered_map<std::uint64_t, Reservation> reservations_;
  std::unordered_map<std::string, std::uint64_t> reservation_keys_;
  std::unordered_map<std::string, std::shared_ptr<const ResidentHandle::Object>> objects_;
};

}  // namespace kvstore::kvcache
