#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "kvstore/common/result.hpp"

namespace kvstore::kvcache {

enum class CachePolicy : std::uint8_t { kLru = 1, kGdsf = 2 };
enum class WorkloadClass : std::uint8_t { kPrefill = 1, kDecode = 2, kLowReuse = 3 };

struct PolicyConfig {
  CachePolicy policy{CachePolicy::kGdsf};
  std::size_t max_tracked_objects{100000};
  std::size_t max_scan_objects{1024};
  double prefill_weight{1.0};
  double decode_weight{1.0};
  double low_reuse_weight{0.25};
};

struct ObjectObservation {
  std::string key;
  std::string tenant;
  std::uint64_t size_bytes{};
  double load_cost{};
  double recompute_cost{};
  std::uint64_t reuse_distance{};
  WorkloadClass workload{WorkloadClass::kPrefill};
  bool resident{true};
};

struct ScoreExplanation {
  double score{};
  double frequency_term{};
  double recency_term{};
  double cost_term{};
  double size_term{};
  double reuse_term{};
  double workload_term{};
  double aging_term{};
  WorkloadClass workload{WorkloadClass::kPrefill};

  friend bool operator==(const ScoreExplanation&, const ScoreExplanation&) = default;
};

struct PolicyMetrics {
  std::uint64_t accesses{};
  std::uint64_t admissions{};
  std::uint64_t rejected_admissions{};
  std::uint64_t evictions{};
  std::uint64_t tracked_objects{};
  std::uint64_t scan_limited{};
};

class CachePolicyEngine {
 public:
  explicit CachePolicyEngine(PolicyConfig config);
  ~CachePolicyEngine();
  [[nodiscard]] Status Validate() const;
  [[nodiscard]] Result<ScoreExplanation> Observe(const ObjectObservation& observation,
                                                 std::uint64_t logical_time);
  [[nodiscard]] Result<ScoreExplanation> Explain(std::string_view key) const;
  [[nodiscard]] Result<bool> ShouldAdmit(const ObjectObservation& observation,
                                         std::size_t resident_objects, std::uint64_t free_bytes);
  [[nodiscard]] Status SetResident(std::string_view key, bool resident);
  [[nodiscard]] Result<std::vector<std::string>> SelectVictims(std::uint64_t bytes_needed,
                                                               std::size_t resident_objects,
                                                               std::uint64_t free_bytes);
  [[nodiscard]] PolicyMetrics Metrics() const;

 private:
  struct Record;
  mutable std::mutex mutex_;
  PolicyConfig config_;
  std::vector<Record> records_;
  std::uint64_t clock_{};
  double aging_{};
  PolicyMetrics metrics_{};
};

enum class LoadMissReason : std::uint8_t {
  kNone = 0,
  kQueueFull = 1,
  kDeadline = 2,
  kCancelled = 3,
  kIoBudget = 4,
};

struct LoadRequest {
  std::uint64_t id{};
  std::string key;
  std::string tenant;
  std::uint64_t bytes{};
  std::uint64_t estimated_io_bytes{};
  std::chrono::nanoseconds estimated_duration{};
  std::chrono::steady_clock::time_point deadline{};
  std::int32_t priority{};
};

struct LoadTask {
  LoadRequest request;
  LoadMissReason miss_reason{LoadMissReason::kNone};

  [[nodiscard]] bool dispatch() const noexcept { return miss_reason == LoadMissReason::kNone; }
};

struct SchedulerConfig {
  SchedulerConfig() = default;
  SchedulerConfig(std::size_t pending_requests, std::size_t concurrent_loads,
                  std::uint64_t inflight_io_bytes, std::uint32_t quantum)
      : max_pending_requests(pending_requests),
        max_concurrent_loads(concurrent_loads),
        max_inflight_io_bytes(inflight_io_bytes),
        tenant_quantum(quantum) {}

  std::size_t max_pending_requests{1024};
  std::size_t max_concurrent_loads{16};
  std::uint64_t max_inflight_io_bytes{256U * 1024U * 1024U};
  std::uint32_t tenant_quantum{1};
  bool fail_activation{false};
};

struct SchedulerMetrics {
  std::uint64_t submitted{};
  std::uint64_t popped{};
  std::uint64_t completed{};
  std::uint64_t rejected_queue_full{};
  std::uint64_t fast_misses{};
  std::uint64_t cancelled{};
  std::uint64_t pending{};
  std::uint64_t inflight_bytes{};
  std::uint64_t max_pending_seen{};
  std::uint64_t tenant_queues{};
};

class LoadScheduler {
 public:
  using Clock = std::chrono::steady_clock;
  explicit LoadScheduler(SchedulerConfig config);
  ~LoadScheduler();
  [[nodiscard]] Status Validate() const;
  [[nodiscard]] Result<LoadMissReason> Submit(LoadRequest request, Clock::time_point now);
  [[nodiscard]] Result<LoadTask> Pop(Clock::time_point now);
  [[nodiscard]] Status Complete(std::uint64_t id, bool success, std::uint64_t actual_io_bytes = 0);
  [[nodiscard]] Status Cancel(std::uint64_t id);
  [[nodiscard]] SchedulerMetrics Metrics() const;

 private:
  struct TenantQueue;
  mutable std::mutex mutex_;
  SchedulerConfig config_;
  std::vector<TenantQueue> tenants_;
  std::size_t pending_count_{};
  std::size_t active_count_{};
  std::uint64_t active_bytes_{};
  std::uint64_t next_tenant_{};
  std::unordered_map<std::uint64_t, std::uint64_t> active_requests_;
  SchedulerMetrics metrics_{};

  void PruneEmptyQueuesLocked();
};

}  // namespace kvstore::kvcache
