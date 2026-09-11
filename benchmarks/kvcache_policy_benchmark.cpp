#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "kvstore/kvcache/policy.hpp"

namespace {

using namespace kvstore::kvcache;

struct Result {
  std::uint64_t requests{};
  std::uint64_t misses{};
  double miss_cost{};
  double p95_latency{};
};

ObjectObservation Object(std::string key, double recompute_cost) {
  return {std::move(key), "tenant", 1, 0, recompute_cost, 1, WorkloadClass::kPrefill};
}

Result Run(CachePolicy policy) {
  CachePolicyEngine engine({policy, 64, 64, 1.0, 1.0, 0.25});
  std::unordered_set<std::string> resident;
  std::vector<ObjectObservation> trace{Object("costly", 100), Object("stable", 1)};
  for (std::size_t cycle = 0; cycle < 12; ++cycle) {
    trace.push_back(Object("scan", 1));
    trace.push_back(Object("stable", 1));
    trace.push_back(Object("scan", 1));
    trace.push_back(Object("stable", 1));
    trace.push_back(Object("scan", 1));
    trace.push_back(Object("costly", 100));
  }

  Result result{trace.size()};
  std::vector<double> latencies;
  std::uint64_t logical_time = 0;
  for (auto observation : trace) {
    ++logical_time;
    if (resident.contains(observation.key)) {
      observation.resident = true;
      if (!engine.Observe(observation, logical_time).ok()) return {};
      latencies.push_back(1.0);
      continue;
    }
    ++result.misses;
    result.miss_cost += observation.recompute_cost;
    latencies.push_back(observation.recompute_cost);
    const std::uint64_t free_bytes = 2U - resident.size();
    auto admit = engine.ShouldAdmit(observation, resident.size(), free_bytes);
    if (!admit.ok()) return {};
    if (!admit.value()) continue;
    if (free_bytes == 0) {
      auto victims = engine.SelectVictims(1, resident.size(), 0);
      if (!victims.ok() || victims.value().empty()) return {};
      resident.erase(victims.value().front());
      if (!engine.SetResident(victims.value().front(), false).ok()) return {};
    }
    resident.insert(observation.key);
    if (!engine.SetResident(observation.key, true).ok()) return {};
  }
  std::sort(latencies.begin(), latencies.end());
  const std::size_t rank = (95U * latencies.size() + 99U) / 100U;
  result.p95_latency = latencies[rank - 1U];
  return result;
}

void Print(std::string_view policy, const Result& result) {
  std::cout << "policy=" << policy << " requests=" << result.requests << " misses=" << result.misses
            << " miss_cost=" << result.miss_cost
            << " deterministic_p95_latency=" << result.p95_latency << '\n';
}

}  // namespace

int main() {
  const auto lru = Run(CachePolicy::kLru);
  const auto gdsf = Run(CachePolicy::kGdsf);
  Print("lru", lru);
  Print("gdsf", gdsf);
  return gdsf.requests != 0 && gdsf.miss_cost < lru.miss_cost && gdsf.p95_latency <= 1.0 &&
                 lru.p95_latency >= 100.0
             ? 0
             : 1;
}
