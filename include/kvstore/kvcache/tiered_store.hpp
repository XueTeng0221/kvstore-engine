#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string_view>
#include <vector>

#include "kvstore/kvcache/model.hpp"

namespace kvstore::kvcache {

enum class TierState : std::uint8_t {
  kResident = 1,
  kLoading = 2,
  kEvicting = 3,
  kDiskOnly = 4,
  kFailed = 5,
};

[[nodiscard]] Status ValidateTierTransition(TierState from, TierState to);

struct ResidentPoolConfig {
  std::uint64_t budget_bytes{};
  std::uint64_t high_watermark_bytes{};
  std::uint64_t low_watermark_bytes{};
};

struct ResidentPoolStats {
  std::uint64_t budget_bytes{};
  std::uint64_t used_bytes{};
  std::uint64_t requested_bytes{};
  std::uint64_t fragmentation_bytes{};
  std::uint64_t peak_used_bytes{};
  std::uint64_t allocation_count{};
  bool above_high_watermark{};
  bool below_low_watermark{};
};

class ResidentAllocation {
 public:
  [[nodiscard]] ByteView bytes() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::uintptr_t address() const noexcept;

 private:
  struct Block;
  explicit ResidentAllocation(std::shared_ptr<Block> block);
  std::shared_ptr<Block> block_;

  friend class ResidentPool;
};

class ResidentPool {
 public:
  struct State;

  explicit ResidentPool(ResidentPoolConfig config);
  [[nodiscard]] Status Validate() const;
  [[nodiscard]] Result<ResidentAllocation> Copy(ByteView bytes);
  [[nodiscard]] ResidentPoolStats Stats() const noexcept;

 private:
  std::shared_ptr<State> state_;
};

class TieredResidentHandle {
 public:
  TieredResidentHandle(const TieredResidentHandle&) = default;
  TieredResidentHandle& operator=(const TieredResidentHandle&) = default;
  // Moved-from handles deliberately retain the shared pin.
  TieredResidentHandle(TieredResidentHandle&& other) noexcept : pin_(other.pin_) {}
  TieredResidentHandle& operator=(TieredResidentHandle&& other) noexcept {
    pin_ = other.pin_;
    return *this;
  }

  [[nodiscard]] const TensorManifest& manifest() const noexcept;
  [[nodiscard]] const CacheKey& key() const noexcept;
  [[nodiscard]] std::size_t chunk_count() const noexcept;
  [[nodiscard]] Result<ByteView> Chunk(std::size_t index) const;

 private:
  struct Pin;
  explicit TieredResidentHandle(std::shared_ptr<const Pin> pin);
  std::shared_ptr<const Pin> pin_;

  friend class TieredStore;
};

struct TieredStoreConfig {
  std::filesystem::path directory;
  ResidentPoolConfig resident;
  std::uint64_t disk_budget_bytes{};
  std::uint64_t max_object_bytes{256U * 1024U * 1024U};
  std::uint32_t max_chunks_per_object{4096};
  std::uint64_t max_metadata_bytes{1U * 1024U * 1024U};
  std::function<Status(std::string_view)> io_fault;
};

struct TieredStoreStats {
  ResidentPoolStats resident;
  std::uint64_t disk_used_bytes{};
  std::uint64_t disk_budget_bytes{};
  std::uint64_t disk_read_count{};
  std::uint64_t object_count{};
};

class TieredStore {
 public:
  using Deadline = std::chrono::steady_clock::time_point;

  [[nodiscard]] static Result<std::unique_ptr<TieredStore>> Open(TieredStoreConfig config);
  ~TieredStore();
  TieredStore(const TieredStore&) = delete;
  TieredStore& operator=(const TieredStore&) = delete;

  [[nodiscard]] Result<TieredResidentHandle> Put(const TensorManifest& manifest,
                                                 const std::vector<ByteView>& chunks);
  [[nodiscard]] Result<TieredResidentHandle> Lookup(const TensorManifest& expected,
                                                    Deadline deadline = Deadline::max(),
                                                    std::stop_token stop_token = {});
  [[nodiscard]] Status Evict(const TensorManifest& expected);
  [[nodiscard]] Status Delete(const TensorManifest& expected);
  [[nodiscard]] Result<TierState> State(const TensorManifest& expected) const;
  [[nodiscard]] TieredStoreStats Stats() const;

 private:
  struct Impl;
  explicit TieredStore(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace kvstore::kvcache
