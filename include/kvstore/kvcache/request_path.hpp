#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <unordered_map>
#include <vector>
#include <thread>
#include <condition_variable>

#include "kvstore/kvcache/match_index.hpp"
#include "kvstore/kvcache/policy.hpp"
#include "kvstore/kvcache/tiered_store.hpp"

namespace kvstore::kvcache {

enum class MatchKind : std::uint8_t { kExact, kPrefix, kMiss };
enum class StorageKind : std::uint8_t { kNone, kMemory, kDisk };

struct LookupRequest {
  TensorManifest query;
  std::vector<std::uint32_t> token_ids;
  std::vector<std::uint64_t> prefix_lengths;
  std::int32_t priority{};
  std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
  std::stop_token cancel;
};

struct LookupMetrics {
  // Match/storage counters count successful callers; miss counts failed lookups
  // after matching starts. Coalesced counts callers joining a shared operation.
  std::uint64_t exact{}, prefix{}, memory{}, disk{}, miss{}, coalesced{};
};

struct LookupResult {
  MatchResult match;
  TieredResidentHandle handle;
  MatchKind match_kind{MatchKind::kMiss};
  StorageKind storage_kind{StorageKind::kNone};
  std::vector<TensorRange> recompute_ranges;
};

class RequestPath {
 public:
  RequestPath(MatchIndex& index, TieredStore& store, SchedulerConfig scheduler_config = {});
  ~RequestPath();
  RequestPath(const RequestPath&) = delete;
  [[nodiscard]] Result<LookupResult> Lookup(const LookupRequest& request);
  [[nodiscard]] LookupMetrics Metrics() const;
  [[nodiscard]] SchedulerMetrics SchedulerState() const;
  void Shutdown() noexcept;

 private:
  struct Pending;
  MatchIndex& index_;
  TieredStore& store_;
  LoadScheduler scheduler_;
  std::mutex shutdown_mutex_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<Pending>> pending_;
  LookupMetrics metrics_{};
  std::condition_variable work_cv_;
  bool stopping_{false};
  std::uint64_t next_id_{};
  std::chrono::milliseconds shutdown_bound_{1000};
  std::thread worker_;
  bool worker_paused_for_test_{false};
  friend class RequestPathTestPeer;
  void Worker();
  void Finish(const std::shared_ptr<Pending>& operation, Result<LookupResult> result);
  void Leave(const std::shared_ptr<Pending>& operation);
};

}  // namespace kvstore::kvcache
