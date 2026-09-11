#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "kvstore/config/config.hpp"
#include "kvstore/kvcache/policy.hpp"

namespace kvstore::kvcache {
namespace {

ObjectObservation Object(std::string key, std::string tenant, std::uint64_t size, double load,
                         double recompute, WorkloadClass workload = WorkloadClass::kPrefill) {
  return {std::move(key), std::move(tenant), size, load, recompute, 1, workload};
}

LoadRequest Request(std::uint64_t id, std::string tenant, std::uint64_t bytes,
                    LoadScheduler::Clock::time_point now, std::int32_t priority = 0) {
  return {id,
          "key-" + std::to_string(id),
          std::move(tenant),
          bytes,
          bytes,
          std::chrono::milliseconds(1),
          now + std::chrono::milliseconds(100),
          priority};
}

struct TraceResult {
  std::uint64_t misses{};
  double miss_cost{};
  double p95_latency{};
};

TraceResult RunTrace(CachePolicy policy) {
  CachePolicyEngine engine({policy, 64, 64, 1.0, 1.0, 0.25});
  std::unordered_set<std::string> resident;
  std::vector<ObjectObservation> trace{Object("costly", "tenant", 1, 0, 100),
                                       Object("stable", "tenant", 1, 0, 1)};
  for (std::size_t cycle = 0; cycle < 12; ++cycle) {
    trace.push_back(Object("scan", "tenant", 1, 0, 1));
    trace.push_back(Object("stable", "tenant", 1, 0, 1));
    trace.push_back(Object("scan", "tenant", 1, 0, 1));
    trace.push_back(Object("stable", "tenant", 1, 0, 1));
    trace.push_back(Object("scan", "tenant", 1, 0, 1));
    trace.push_back(Object("costly", "tenant", 1, 0, 100));
  }

  TraceResult result;
  std::vector<double> latencies;
  std::uint64_t logical_time = 0;
  for (auto observation : trace) {
    ++logical_time;
    if (resident.contains(observation.key)) {
      observation.resident = true;
      EXPECT_TRUE(engine.Observe(observation, logical_time).ok());
      latencies.push_back(1.0);
      continue;
    }

    ++result.misses;
    result.miss_cost += observation.recompute_cost;
    latencies.push_back(observation.recompute_cost);
    const std::uint64_t free_bytes = 2U - resident.size();
    const auto admit = engine.ShouldAdmit(observation, resident.size(), free_bytes);
    EXPECT_TRUE(admit.ok());
    if (!admit.ok() || !admit.value()) continue;
    if (free_bytes == 0) {
      const auto victims = engine.SelectVictims(1, resident.size(), 0);
      EXPECT_TRUE(victims.ok());
      if (!victims.ok() || victims.value().empty()) continue;
      const auto& victim = victims.value().front();
      resident.erase(victim);
      EXPECT_TRUE(engine.SetResident(victim, false).ok());
    }
    resident.insert(observation.key);
    EXPECT_TRUE(engine.SetResident(observation.key, true).ok());
  }
  std::sort(latencies.begin(), latencies.end());
  const std::size_t rank = (95U * latencies.size() + 99U) / 100U;
  result.p95_latency = latencies[rank - 1U];
  return result;
}

TEST(KvCachePolicyTest, ValidatesBoundsEnumsAndExplainsAllSignals) {
  CachePolicyEngine engine({CachePolicy::kGdsf, 4, 2, 1.0, 2.0, 0.25});
  EXPECT_TRUE(engine.Validate().ok());
  const auto score = engine.Observe(Object("a", "t", 100, 3.0, 7.0, WorkloadClass::kDecode), 10);
  ASSERT_TRUE(score.ok());
  EXPECT_EQ(score.value().frequency_term, 1.0);
  EXPECT_EQ(score.value().cost_term, 10.0);
  EXPECT_EQ(score.value().size_term, 100.0);
  EXPECT_EQ(score.value().reuse_term, 0.5);
  EXPECT_EQ(score.value().workload_term, 2.0);
  EXPECT_EQ(score.value().workload, WorkloadClass::kDecode);

  CachePolicyEngine bad_policy({static_cast<CachePolicy>(255), 1, 1, 1.0, 1.0, 1.0});
  EXPECT_EQ(bad_policy.Validate().code(), StatusCode::kInvalidArgument);
  auto bad_workload = Object("bad", "t", 1, 1, 1);
  bad_workload.workload = static_cast<WorkloadClass>(255);
  EXPECT_EQ(engine.Observe(bad_workload, 11).status().code(), StatusCode::kInvalidArgument);
}

TEST(KvCachePolicyTest, ClampsFiniteInputOverflowAndRejectsNonFiniteInput) {
  const double maximum = std::numeric_limits<double>::max();
  CachePolicyEngine engine({CachePolicy::kGdsf, 4, 4, maximum, 1.0, 1.0});
  const auto score = engine.Observe(Object("large", "t", 1, maximum, maximum), 1);
  ASSERT_TRUE(score.ok());
  EXPECT_TRUE(std::isfinite(score.value().score));
  EXPECT_EQ(score.value().score, maximum);
  EXPECT_EQ(score.value().cost_term, maximum);

  auto invalid = Object("nan", "t", 1, 1, 1);
  invalid.load_cost = std::numeric_limits<double>::infinity();
  EXPECT_EQ(engine.Observe(invalid, 2).status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(engine.Metrics().accesses, 1U);
}

TEST(KvCachePolicyTest, RejectedAdmissionPreservesExistingObjectAndTrackedState) {
  CachePolicyEngine engine({CachePolicy::kGdsf, 1, 1, 1.0, 1.0, 0.0});
  ASSERT_TRUE(engine.Observe(Object("resident", "t", 10, 1, 10), 1).ok());
  const auto before = engine.Explain("resident").value();
  const auto rejected =
      engine.ShouldAdmit(Object("resident", "t", 20, 0, 0, WorkloadClass::kLowReuse), 1, 0);
  ASSERT_TRUE(rejected.ok());
  EXPECT_FALSE(rejected.value());
  EXPECT_EQ(engine.Explain("resident").value(), before);
  const auto victims = engine.SelectVictims(10, 1, 0);
  ASSERT_TRUE(victims.ok());
  ASSERT_EQ(victims.value().size(), 1U);
  EXPECT_EQ(victims.value().front(), "resident");
  EXPECT_EQ(engine.Metrics().tracked_objects, 1U);
}

TEST(KvCachePolicyTest, BoundsScanTrackingAndLowReusePollution) {
  CachePolicyEngine engine({CachePolicy::kGdsf, 8, 2, 1.0, 1.0, 0.1});
  for (std::uint64_t index = 0; index < 10; ++index) {
    ASSERT_TRUE(engine.Observe(Object(std::to_string(index), "t", 10, 1, 1), index).ok());
  }
  const auto victims = engine.SelectVictims(100, 8, 0);
  ASSERT_TRUE(victims.ok());
  EXPECT_EQ(victims.value().size(), 2U);
  EXPECT_EQ(engine.Metrics().scan_limited, 1U);
  EXPECT_EQ(engine.Metrics().tracked_objects, 8U);
  const auto pollution =
      engine.ShouldAdmit(Object("pollution", "t", 10, 1, 1, WorkloadClass::kLowReuse), 8, 0);
  ASSERT_TRUE(pollution.ok());
  EXPECT_FALSE(pollution.value());
  EXPECT_FALSE(engine.Explain("pollution").ok());
}

TEST(KvCachePolicyTest, DeterministicTraceShowsGdsfCostAndP95Benefit) {
  const auto lru = RunTrace(CachePolicy::kLru);
  const auto gdsf = RunTrace(CachePolicy::kGdsf);
  EXPECT_LT(gdsf.miss_cost, lru.miss_cost);
  EXPECT_GT(gdsf.misses, lru.misses);
  EXPECT_LE(gdsf.p95_latency, 1.0);
  EXPECT_GE(lru.p95_latency, 100.0);
}

TEST(KvCacheSchedulerTest, ValidatesQueueDeadlineAndMetricsBounds) {
  const auto now = LoadScheduler::Clock::time_point{};
  LoadScheduler scheduler({1, 1, 10, 1});
  EXPECT_TRUE(scheduler.Validate().ok());
  EXPECT_EQ(scheduler.Submit(Request(1, "a", 10, now), now).value(), LoadMissReason::kNone);
  EXPECT_EQ(scheduler.Submit(Request(2, "a", 10, now), now).value(), LoadMissReason::kQueueFull);
  EXPECT_EQ(scheduler.Submit(Request(3, "a", 1, now), now + std::chrono::seconds(1)).value(),
            LoadMissReason::kDeadline);
  EXPECT_EQ(scheduler.Metrics().max_pending_seen, 1U);
}

TEST(KvCacheSchedulerTest, InjectedActivationFailureDoesNotLeakStateOrDeadlockMetrics) {
  const auto now = LoadScheduler::Clock::time_point{};
  SchedulerConfig config{4, 1, 10, 1};
  config.fail_activation = true;
  LoadScheduler scheduler(std::move(config));
  ASSERT_TRUE(scheduler.Submit(Request(1, "a", 6, now), now).ok());
  EXPECT_EQ(scheduler.Pop(now).status().code(), StatusCode::kLimitExceeded);
  const auto metrics = scheduler.Metrics();
  EXPECT_EQ(metrics.pending, 1U);
  EXPECT_EQ(metrics.popped, 0U);
  EXPECT_EQ(metrics.inflight_bytes, 0U);
  EXPECT_EQ(metrics.tenant_queues, 1U);
}

TEST(KvCacheSchedulerTest, ExtremeDeadlinesDoNotOverflowFeasibilityCheck) {
  const auto now = LoadScheduler::Clock::time_point{};
  LoadScheduler scheduler({4, 1, 10, 1});
  auto request = Request(1, "a", 1, now);
  request.deadline = LoadScheduler::Clock::time_point::max();
  request.estimated_duration = std::chrono::nanoseconds::max();
  EXPECT_EQ(scheduler.Submit(request, now).value(), LoadMissReason::kNone);
  EXPECT_TRUE(scheduler.Cancel(1).ok());

  request.id = 2;
  request.deadline = LoadScheduler::Clock::time_point::min();
  request.estimated_duration = std::chrono::nanoseconds::zero();
  EXPECT_EQ(scheduler.Submit(request, now).value(), LoadMissReason::kDeadline);

  request.id = 3;
  const auto near_max = LoadScheduler::Clock::time_point::max() - std::chrono::nanoseconds(1);
  request.deadline = LoadScheduler::Clock::time_point::max();
  request.estimated_duration = std::chrono::nanoseconds(2);
  EXPECT_EQ(scheduler.Submit(request, near_max).value(), LoadMissReason::kDeadline);
}

TEST(KvCacheSchedulerTest, SkipsBlockedPriorityAndSelectsEligibleRequest) {
  const auto now = LoadScheduler::Clock::time_point{};
  LoadScheduler scheduler({8, 2, 10, 1});
  ASSERT_TRUE(scheduler.Submit(Request(1, "leader", 6, now, 100), now).ok());
  ASSERT_TRUE(scheduler.Submit(Request(2, "tenant", 6, now, 100), now).ok());
  ASSERT_TRUE(scheduler.Submit(Request(3, "tenant", 4, now, 1), now).ok());
  ASSERT_EQ(scheduler.Pop(now).value().request.id, 1U);
  const auto eligible = scheduler.Pop(now);
  ASSERT_TRUE(eligible.ok());
  EXPECT_EQ(eligible.value().request.id, 3U);
  EXPECT_TRUE(scheduler.Complete(1, true, 6).ok());
  EXPECT_TRUE(scheduler.Complete(3, true, 4).ok());
}

TEST(KvCacheSchedulerTest, ExpiresBlockedWorkAndStillDispatchesEligibleDeadline) {
  const auto start = LoadScheduler::Clock::time_point{};
  LoadScheduler scheduler({8, 1, 100, 1});
  auto expired = Request(1, "a", 1, start, 100);
  auto eligible = Request(2, "a", 1, start, 1);
  eligible.deadline = start + std::chrono::seconds(2);
  ASSERT_TRUE(scheduler.Submit(expired, start).ok());
  ASSERT_TRUE(scheduler.Submit(eligible, start).ok());
  const auto task = scheduler.Pop(start + std::chrono::milliseconds(200));
  ASSERT_TRUE(task.ok());
  EXPECT_TRUE(task.value().dispatch());
  EXPECT_EQ(task.value().request.id, 2U);
  EXPECT_EQ(scheduler.Metrics().fast_misses, 1U);
}

TEST(KvCacheSchedulerTest, FairlyAlternatesTenantsHonorsQuantumAndCancellation) {
  const auto now = LoadScheduler::Clock::time_point{};
  LoadScheduler scheduler({8, 1, 100, 2});
  for (std::uint64_t id : {1U, 2U, 3U}) {
    ASSERT_TRUE(scheduler.Submit(Request(id, "a", 1, now), now).ok());
  }
  ASSERT_TRUE(scheduler.Submit(Request(4, "b", 1, now), now).ok());
  EXPECT_EQ(scheduler.Pop(now).value().request.tenant, "a");
  EXPECT_TRUE(scheduler.Complete(1, true).ok());
  EXPECT_EQ(scheduler.Pop(now).value().request.tenant, "a");
  EXPECT_TRUE(scheduler.Complete(2, true).ok());
  EXPECT_EQ(scheduler.Pop(now).value().request.tenant, "b");
  EXPECT_TRUE(scheduler.Complete(4, true).ok());
  EXPECT_TRUE(scheduler.Cancel(3).ok());
  EXPECT_EQ(scheduler.Metrics().cancelled, 1U);
}

TEST(KvCachePolicySchedulerTest, ConcurrentOperationsHaveConsistentSnapshots) {
  constexpr std::size_t kThreads = 4;
  constexpr std::size_t kOperations = 50;
  CachePolicyEngine policy({CachePolicy::kGdsf, 16, 16, 1.0, 1.0, 0.25});
  LoadScheduler scheduler({kThreads * kOperations, kThreads, 1024, 1});
  const auto now = LoadScheduler::Clock::time_point{};
  std::barrier start(static_cast<std::ptrdiff_t>(kThreads));
  std::atomic<bool> failed{false};
  std::vector<std::thread> producers;
  for (std::size_t thread = 0; thread < kThreads; ++thread) {
    producers.emplace_back([&, thread] {
      start.arrive_and_wait();
      for (std::size_t operation = 0; operation < kOperations; ++operation) {
        if (!policy
                 .Observe(Object("object-" + std::to_string(thread), "tenant", 1, 1, 1), operation)
                 .ok()) {
          failed.store(true, std::memory_order_relaxed);
        }
        const auto id = static_cast<std::uint64_t>(thread * kOperations + operation + 1U);
        auto request = Request(id, "tenant-" + std::to_string(thread), 1, now);
        request.deadline = now + std::chrono::seconds(10);
        if (!scheduler.Submit(std::move(request), now).ok()) {
          failed.store(true, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& producer : producers) producer.join();
  ASSERT_FALSE(failed.load(std::memory_order_relaxed));

  std::atomic<std::size_t> completed{0};
  std::vector<std::thread> consumers;
  for (std::size_t thread = 0; thread < kThreads; ++thread) {
    consumers.emplace_back([&] {
      while (completed.load(std::memory_order_relaxed) < kThreads * kOperations) {
        auto task = scheduler.Pop(now);
        if (!task.ok()) {
          std::this_thread::yield();
          continue;
        }
        if (!scheduler.Complete(task.value().request.id, true, 1).ok()) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
        completed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& consumer : consumers) consumer.join();
  EXPECT_FALSE(failed.load(std::memory_order_relaxed));
  EXPECT_EQ(policy.Metrics().accesses, kThreads * kOperations);
  EXPECT_EQ(scheduler.Metrics().completed, kThreads * kOperations);
  EXPECT_EQ(scheduler.Metrics().pending, 0U);
}

TEST(KvCacheSchedulerTest, RejectsDuplicateIdsAndInvalidConfiguration) {
  const auto now = LoadScheduler::Clock::time_point{};
  LoadScheduler scheduler({4, 1, 100, 1});
  ASSERT_TRUE(scheduler.Submit(Request(1, "a", 1, now), now).ok());
  EXPECT_EQ(scheduler.Submit(Request(1, "b", 1, now), now).status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(LoadScheduler({0, 1, 1, 1}).Validate().code(), StatusCode::kInvalidArgument);
}

TEST(KvCacheSchedulerTest, PrunesEmptyTenantMetadataWithoutChangingCursor) {
  const auto now = LoadScheduler::Clock::time_point{};
  LoadScheduler scheduler({16, 1, 100, 1});
  for (std::uint64_t id = 1; id <= 32; ++id) {
    ASSERT_TRUE(scheduler.Submit(Request(id, "tenant-" + std::to_string(id), 1, now), now).ok());
    const auto task = scheduler.Pop(now);
    ASSERT_TRUE(task.ok());
    EXPECT_EQ(scheduler.Metrics().tenant_queues, 0U);
    ASSERT_TRUE(scheduler.Complete(task.value().request.id, true, 1).ok());
  }
  EXPECT_EQ(scheduler.Metrics().pending, 0U);
  EXPECT_EQ(scheduler.Pop(now).status().code(), StatusCode::kNotFound);
  EXPECT_EQ(scheduler.Metrics().tenant_queues, 0U);
  EXPECT_TRUE(scheduler.Cancel(999).code() == StatusCode::kInvalidArgument);
}

TEST(ConfigTest, LoadsP74SchedulerPolicyConfiguration) {
  const auto result =
      Config::Load(std::filesystem::path(KVSTORE_SOURCE_DIR) / "config/config.json");
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result.value().kvcache.admission_policy, "gdsf");
  EXPECT_EQ(result.value().kvcache.max_pending_loads, 1024U);
  EXPECT_EQ(result.value().kvcache.max_inflight_io_bytes, 268435456U);
  EXPECT_EQ(result.value().kvcache.max_tracked_objects, 100000U);
  EXPECT_EQ(result.value().kvcache.decode_weight, 2.0);
}

}  // namespace
}  // namespace kvstore::kvcache
