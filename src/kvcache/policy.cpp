#include "kvstore/kvcache/policy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace kvstore::kvcache {
namespace {

void SaturatingIncrement(std::uint64_t& value) {
  if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

std::uint64_t Incremented(std::uint64_t value) {
  SaturatingIncrement(value);
  return value;
}

bool ValidPolicy(CachePolicy policy) {
  switch (policy) {
    case CachePolicy::kLru:
    case CachePolicy::kGdsf:
      return true;
  }
  return false;
}

Result<double> WorkloadWeight(const PolicyConfig& config, WorkloadClass workload) {
  switch (workload) {
    case WorkloadClass::kPrefill:
      return config.prefill_weight;
    case WorkloadClass::kDecode:
      return config.decode_weight;
    case WorkloadClass::kLowReuse:
      return config.low_reuse_weight;
  }
  return Status{StatusCode::kInvalidArgument, "unknown workload class"};
}

Status Invalid(std::string message) { return {StatusCode::kInvalidArgument, std::move(message)}; }

bool CanFinish(LoadScheduler::Clock::time_point now, LoadScheduler::Clock::time_point deadline,
               std::chrono::nanoseconds duration) {
  if (duration < std::chrono::nanoseconds::zero() || deadline <= now) return false;
  const auto now_ticks = now.time_since_epoch().count();
  const auto deadline_ticks = deadline.time_since_epoch().count();
  using Rep = std::remove_cv_t<decltype(now_ticks)>;
  using UnsignedRep = std::make_unsigned_t<Rep>;
  const UnsignedRep available_ticks =
      static_cast<UnsignedRep>(deadline_ticks) - static_cast<UnsignedRep>(now_ticks);
  const long double available_seconds = static_cast<long double>(available_ticks) *
                                        LoadScheduler::Clock::period::num /
                                        LoadScheduler::Clock::period::den;
  const long double required_seconds =
      static_cast<long double>(duration.count()) / std::chrono::nanoseconds::period::den;
  return required_seconds <= available_seconds;
}

Status ValidateObservation(const ObjectObservation& observation) {
  if (observation.key.empty() || observation.tenant.empty() || observation.size_bytes == 0) {
    return Invalid("policy observation identity and size must be non-empty");
  }
  if (!std::isfinite(observation.load_cost) || !std::isfinite(observation.recompute_cost) ||
      observation.load_cost < 0.0 || observation.recompute_cost < 0.0) {
    return Invalid("policy costs must be finite and non-negative");
  }
  if (!WorkloadWeight(PolicyConfig{}, observation.workload).ok()) {
    return Invalid("unknown workload class");
  }
  return Status::Ok();
}

Result<double> ClampScore(long double value, std::string_view term) {
  if (std::isnan(value) || value < 0.0L) {
    return Invalid(std::string(term) + " is invalid");
  }
  constexpr long double kMaximum = static_cast<long double>(std::numeric_limits<double>::max());
  if (!std::isfinite(value) || value > kMaximum) return std::numeric_limits<double>::max();
  return static_cast<double>(value);
}

Result<ScoreExplanation> ComputeScore(const PolicyConfig& config,
                                      const ObjectObservation& observation, std::uint64_t frequency,
                                      std::uint64_t last_access, std::uint64_t now, double aging) {
  const auto observation_status = ValidateObservation(observation);
  if (!observation_status.ok()) return observation_status;
  if (!std::isfinite(aging) || aging < 0.0) return Invalid("policy aging is invalid");
  const auto workload = WorkloadWeight(config, observation.workload);
  if (!workload.ok()) return workload.status();

  const std::uint64_t age_ticks = now >= last_access ? now - last_access : 0;
  const long double recency_value = 1.0L / (1.0L + static_cast<long double>(age_ticks));
  const long double cost_value = static_cast<long double>(observation.load_cost) +
                                 static_cast<long double>(observation.recompute_cost);
  const long double size_value = static_cast<long double>(observation.size_bytes);
  const long double reuse_value =
      1.0L / (1.0L + static_cast<long double>(observation.reuse_distance));
  const long double frequency_value = static_cast<long double>(frequency);
  const long double policy_value =
      config.policy == CachePolicy::kLru
          ? recency_value
          : static_cast<long double>(aging) +
                frequency_value * workload.value() * cost_value * reuse_value / size_value;

  auto score = ClampScore(policy_value, "policy score");
  auto frequency_term = ClampScore(frequency_value, "frequency term");
  auto recency_term = ClampScore(recency_value, "recency term");
  auto cost_term = ClampScore(cost_value, "cost term");
  auto size_term = ClampScore(size_value, "size term");
  auto reuse_term = ClampScore(reuse_value, "reuse term");
  if (!score.ok()) return score.status();
  if (!frequency_term.ok()) return frequency_term.status();
  if (!recency_term.ok()) return recency_term.status();
  if (!cost_term.ok()) return cost_term.status();
  if (!size_term.ok()) return size_term.status();
  if (!reuse_term.ok()) return reuse_term.status();
  return ScoreExplanation{
      score.value(),       frequency_term.value(), recency_term.value(), cost_term.value(),
      size_term.value(),   reuse_term.value(),     workload.value(),     aging,
      observation.workload};
}

}  // namespace

struct CachePolicyEngine::Record {
  ObjectObservation observation;
  std::uint64_t last_access{};
  std::uint64_t frequency{};
  ScoreExplanation score;
};

CachePolicyEngine::CachePolicyEngine(PolicyConfig config) : config_(std::move(config)) {}
CachePolicyEngine::~CachePolicyEngine() = default;

Status CachePolicyEngine::Validate() const {
  if (!ValidPolicy(config_.policy)) return Invalid("unknown cache policy");
  if (config_.max_tracked_objects == 0 || config_.max_scan_objects == 0) {
    return Invalid("policy object limits must be positive");
  }
  if (!std::isfinite(config_.prefill_weight) || !std::isfinite(config_.decode_weight) ||
      !std::isfinite(config_.low_reuse_weight) || config_.prefill_weight < 0.0 ||
      config_.decode_weight < 0.0 || config_.low_reuse_weight < 0.0) {
    return Invalid("policy workload weights must be finite and non-negative");
  }
  return Status::Ok();
}

Result<ScoreExplanation> CachePolicyEngine::Observe(const ObjectObservation& observation,
                                                    std::uint64_t logical_time) {
  const auto config_status = Validate();
  if (!config_status.ok()) return config_status;
  const auto observation_status = ValidateObservation(observation);
  if (!observation_status.ok()) return observation_status;

  std::lock_guard lock(mutex_);
  const std::uint64_t next_clock = std::max(Incremented(clock_), logical_time);
  auto found = std::find_if(records_.begin(), records_.end(), [&](const Record& record) {
    return record.observation.key == observation.key;
  });
  const std::uint64_t frequency = found == records_.end() ? 1 : Incremented(found->frequency);
  const std::uint64_t last_access = found == records_.end() ? next_clock : found->last_access;
  auto score = ComputeScore(config_, observation, frequency, last_access, next_clock, aging_);
  if (!score.ok()) return score.status();

  try {
    Record prepared{observation, next_clock, frequency, score.value()};
    if (found == records_.end()) {
      if (records_.size() < config_.max_tracked_objects) {
        records_.push_back(std::move(prepared));
      } else {
        const auto victim = std::min_element(
            records_.begin(), records_.end(), [](const Record& left, const Record& right) {
              if (left.observation.resident != right.observation.resident) {
                return !left.observation.resident;
              }
              return left.last_access < right.last_access;
            });
        *victim = std::move(prepared);
      }
    } else {
      *found = std::move(prepared);
    }
  } catch (const std::bad_alloc&) {
    return Status{StatusCode::kLimitExceeded, "policy observation allocation failed"};
  }
  clock_ = next_clock;
  SaturatingIncrement(metrics_.accesses);
  metrics_.tracked_objects = records_.size();
  return score.value();
}

Result<ScoreExplanation> CachePolicyEngine::Explain(std::string_view key) const {
  std::lock_guard lock(mutex_);
  const auto found = std::find_if(records_.begin(), records_.end(), [&](const Record& record) {
    return record.observation.key == key;
  });
  if (found == records_.end()) return Status{StatusCode::kNotFound, "policy object not found"};
  return found->score;
}

Result<bool> CachePolicyEngine::ShouldAdmit(const ObjectObservation& observation,
                                            std::size_t resident_objects,
                                            std::uint64_t free_bytes) {
  const auto config_status = Validate();
  if (!config_status.ok()) return config_status;
  const auto observation_status = ValidateObservation(observation);
  if (!observation_status.ok()) return observation_status;

  std::lock_guard lock(mutex_);
  const std::uint64_t next_clock = Incremented(clock_);
  auto found = std::find_if(records_.begin(), records_.end(), [&](const Record& record) {
    return record.observation.key == observation.key;
  });
  const std::uint64_t frequency = found == records_.end() ? 1 : Incremented(found->frequency);
  const std::uint64_t last_access = found == records_.end() ? next_clock : found->last_access;
  auto candidate = observation;
  candidate.resident = found != records_.end() && found->observation.resident;
  auto score = ComputeScore(config_, candidate, frequency, last_access, next_clock, aging_);
  if (!score.ok()) return score.status();

  const bool reusable = observation.workload != WorkloadClass::kLowReuse || frequency >= 2;
  bool admit = reusable && observation.size_bytes <= free_bytes;
  if (reusable && !admit && resident_objects != 0) {
    double victim_score = std::numeric_limits<double>::infinity();
    std::size_t inspected = 0;
    for (const auto& record : records_) {
      if (inspected++ >= config_.max_scan_objects) break;
      if (!record.observation.resident || record.observation.key == observation.key) continue;
      auto current = ComputeScore(config_, record.observation, record.frequency, record.last_access,
                                  next_clock, aging_);
      if (!current.ok()) return current.status();
      victim_score = std::min(victim_score, current.value().score);
    }
    admit = score.value().score > victim_score;
  }
  if (!admit) {
    SaturatingIncrement(metrics_.rejected_admissions);
    return false;
  }

  try {
    Record prepared{candidate, next_clock, frequency, score.value()};
    if (found == records_.end()) {
      if (records_.size() < config_.max_tracked_objects) {
        records_.push_back(std::move(prepared));
      } else {
        const auto victim = std::min_element(
            records_.begin(), records_.end(), [](const Record& left, const Record& right) {
              if (left.observation.resident != right.observation.resident) {
                return !left.observation.resident;
              }
              return left.last_access < right.last_access;
            });
        *victim = std::move(prepared);
      }
    } else {
      *found = std::move(prepared);
    }
  } catch (const std::bad_alloc&) {
    return Status{StatusCode::kLimitExceeded, "policy admission allocation failed"};
  }
  clock_ = next_clock;
  SaturatingIncrement(metrics_.accesses);
  SaturatingIncrement(metrics_.admissions);
  metrics_.tracked_objects = records_.size();
  return true;
}

Status CachePolicyEngine::SetResident(std::string_view key, bool resident) {
  std::lock_guard lock(mutex_);
  const auto found = std::find_if(records_.begin(), records_.end(), [&](const Record& record) {
    return record.observation.key == key;
  });
  if (found == records_.end()) return {StatusCode::kNotFound, "policy object not found"};
  found->observation.resident = resident;
  return Status::Ok();
}

Result<std::vector<std::string>> CachePolicyEngine::SelectVictims(std::uint64_t bytes_needed,
                                                                  std::size_t resident_objects,
                                                                  std::uint64_t free_bytes) {
  const auto config_status = Validate();
  if (!config_status.ok()) return config_status;
  std::lock_guard lock(mutex_);
  if (bytes_needed <= free_bytes) return std::vector<std::string>{};

  struct Candidate {
    const Record* record;
    double score;
  };
  try {
    std::vector<Candidate> candidates;
    candidates.reserve(std::min(config_.max_scan_objects, records_.size()));
    std::size_t scanned = 0;
    for (std::size_t index = 0; index < records_.size() && scanned < config_.max_scan_objects;
         ++index, ++scanned) {
      const auto& record = records_[index];
      if (!record.observation.resident) continue;
      auto score = ComputeScore(config_, record.observation, record.frequency, record.last_access,
                                clock_, aging_);
      if (!score.ok()) return score.status();
      candidates.push_back({&record, score.value().score});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                if (left.score != right.score) return left.score < right.score;
                if (left.record->last_access != right.record->last_access) {
                  return left.record->last_access < right.record->last_access;
                }
                return left.record->observation.key < right.record->observation.key;
              });

    std::vector<std::string> result;
    result.reserve(std::min(resident_objects, candidates.size()));
    std::uint64_t reclaimed = free_bytes;
    double next_aging = aging_;
    for (const auto& candidate : candidates) {
      if (reclaimed >= bytes_needed || result.size() >= resident_objects) break;
      result.push_back(candidate.record->observation.key);
      if (config_.policy == CachePolicy::kGdsf) next_aging = std::max(next_aging, candidate.score);
      const auto size = candidate.record->observation.size_bytes;
      reclaimed = size > std::numeric_limits<std::uint64_t>::max() - reclaimed
                      ? std::numeric_limits<std::uint64_t>::max()
                      : reclaimed + size;
    }
    aging_ = next_aging;
    if (scanned < records_.size()) SaturatingIncrement(metrics_.scan_limited);
    for (std::size_t index = 0; index < result.size(); ++index) {
      SaturatingIncrement(metrics_.evictions);
    }
    return result;
  } catch (const std::bad_alloc&) {
    return Status{StatusCode::kLimitExceeded, "policy victim selection allocation failed"};
  }
}

PolicyMetrics CachePolicyEngine::Metrics() const {
  std::lock_guard lock(mutex_);
  return metrics_;
}

struct LoadScheduler::TenantQueue {
  std::string tenant;
  std::vector<LoadRequest> pending;
  std::uint64_t served{};
};

void LoadScheduler::PruneEmptyQueuesLocked() {
  for (std::size_t index = 0; index < tenants_.size();) {
    if (!tenants_[index].pending.empty()) {
      ++index;
      continue;
    }
    if (next_tenant_ > index) --next_tenant_;
    tenants_.erase(tenants_.begin() + static_cast<std::ptrdiff_t>(index));
  }
  next_tenant_ = tenants_.empty() ? 0 : next_tenant_ % tenants_.size();
  metrics_.tenant_queues = tenants_.size();
}

LoadScheduler::LoadScheduler(SchedulerConfig config) : config_(std::move(config)) {}
LoadScheduler::~LoadScheduler() = default;

Status LoadScheduler::Validate() const {
  if (config_.max_pending_requests == 0 || config_.max_concurrent_loads == 0 ||
      config_.max_inflight_io_bytes == 0 || config_.tenant_quantum == 0) {
    return Invalid("scheduler limits must be positive");
  }
  return Status::Ok();
}

Result<LoadMissReason> LoadScheduler::Submit(LoadRequest request, Clock::time_point now) {
  const auto config_status = Validate();
  if (!config_status.ok()) return config_status;
  if (request.tenant.empty() || request.key.empty() || request.id == 0 || request.bytes == 0 ||
      request.estimated_io_bytes == 0 || request.estimated_duration < Clock::duration::zero()) {
    return Invalid("invalid load request");
  }

  std::lock_guard lock(mutex_);
  if (!CanFinish(now, request.deadline, request.estimated_duration)) {
    SaturatingIncrement(metrics_.fast_misses);
    return LoadMissReason::kDeadline;
  }
  if (pending_count_ >= config_.max_pending_requests) {
    SaturatingIncrement(metrics_.rejected_queue_full);
    return LoadMissReason::kQueueFull;
  }
  if (active_requests_.contains(request.id)) return Invalid("duplicate load request id");
  for (const auto& queue : tenants_) {
    if (std::any_of(queue.pending.begin(), queue.pending.end(),
                    [&](const LoadRequest& pending) { return pending.id == request.id; })) {
      return Invalid("duplicate load request id");
    }
  }
  if (request.estimated_io_bytes > config_.max_inflight_io_bytes ||
      active_bytes_ > config_.max_inflight_io_bytes - request.estimated_io_bytes) {
    SaturatingIncrement(metrics_.fast_misses);
    return LoadMissReason::kIoBudget;
  }

  try {
    auto found = std::find_if(tenants_.begin(), tenants_.end(), [&](const TenantQueue& queue) {
      return queue.tenant == request.tenant;
    });
    if (found == tenants_.end()) {
      TenantQueue prepared{request.tenant, {}, 0};
      prepared.pending.push_back(std::move(request));
      tenants_.push_back(std::move(prepared));
    } else {
      found->pending.push_back(std::move(request));
    }
  } catch (const std::bad_alloc&) {
    return Status{StatusCode::kLimitExceeded, "load queue allocation failed"};
  }
  ++pending_count_;
  SaturatingIncrement(metrics_.submitted);
  metrics_.pending = pending_count_;
  metrics_.max_pending_seen = std::max(metrics_.max_pending_seen, metrics_.pending);
  metrics_.tenant_queues = tenants_.size();
  return LoadMissReason::kNone;
}

Result<LoadTask> LoadScheduler::Pop(Clock::time_point now) {
  const auto config_status = Validate();
  if (!config_status.ok()) return config_status;
  std::lock_guard lock(mutex_);
  PruneEmptyQueuesLocked();
  if (active_count_ >= config_.max_concurrent_loads) {
    return Status{StatusCode::kBusy, "load concurrency limit"};
  }
  if (pending_count_ == 0) return Status{StatusCode::kNotFound, "load queue is empty"};

  std::optional<LoadTask> first_expired;
  const std::size_t tenant_count = tenants_.size();
  for (std::size_t offset = 0; offset < tenant_count; ++offset) {
    const std::size_t tenant_index =
        static_cast<std::size_t>((next_tenant_ + offset) % tenant_count);
    auto& queue = tenants_[tenant_index];
    std::optional<std::size_t> choice;
    for (std::size_t index = 0; index < queue.pending.size();) {
      const auto& request = queue.pending[index];
      const bool expired = !CanFinish(now, request.deadline, request.estimated_duration);
      if (expired) {
        try {
          if (!first_expired.has_value()) {
            first_expired.emplace(LoadTask{request, LoadMissReason::kDeadline});
          }
        } catch (const std::bad_alloc&) {
          return Status{StatusCode::kLimitExceeded, "expired load result allocation failed"};
        }
        queue.pending.erase(queue.pending.begin() + static_cast<std::ptrdiff_t>(index));
        --pending_count_;
        SaturatingIncrement(metrics_.fast_misses);
        metrics_.pending = pending_count_;
        continue;
      }
      const bool fits = request.estimated_io_bytes <= config_.max_inflight_io_bytes - active_bytes_;
      if (fits && (!choice.has_value() || request.priority > queue.pending[*choice].priority ||
                   (request.priority == queue.pending[*choice].priority &&
                    (request.deadline < queue.pending[*choice].deadline ||
                     (request.deadline == queue.pending[*choice].deadline &&
                      request.id < queue.pending[*choice].id))))) {
        choice = index;
      }
      ++index;
    }
    if (!choice.has_value()) continue;

    LoadTask task;
    try {
      task = LoadTask{queue.pending[*choice], LoadMissReason::kNone};
      if (config_.fail_activation) {
        return Status{StatusCode::kLimitExceeded, "injected activation failure"};
      }
      const auto [unused, inserted] =
          active_requests_.emplace(task.request.id, task.request.estimated_io_bytes);
      static_cast<void>(unused);
      if (!inserted) return Status{StatusCode::kInternal, "duplicate active load id"};
    } catch (const std::bad_alloc&) {
      return Status{StatusCode::kLimitExceeded, "active load allocation failed"};
    } catch (...) {
      return Status{StatusCode::kInternal, "active load activation failed"};
    }

    queue.pending.erase(queue.pending.begin() + static_cast<std::ptrdiff_t>(*choice));
    --pending_count_;
    active_bytes_ += task.request.estimated_io_bytes;
    ++active_count_;
    ++queue.served;
    if (queue.served >= config_.tenant_quantum || queue.pending.empty()) {
      queue.served = 0;
      next_tenant_ = (tenant_index + 1) % tenant_count;
    } else {
      next_tenant_ = tenant_index;
    }
    SaturatingIncrement(metrics_.popped);
    metrics_.pending = pending_count_;
    metrics_.inflight_bytes = active_bytes_;
    PruneEmptyQueuesLocked();
    return task;
  }
  PruneEmptyQueuesLocked();
  if (first_expired.has_value()) return std::move(*first_expired);
  return Status{StatusCode::kBusy, "no eligible load within I/O budget"};
}

Status LoadScheduler::Complete(std::uint64_t id, bool /*success*/, std::uint64_t actual_io_bytes) {
  const auto config_status = Validate();
  if (!config_status.ok()) return config_status;
  std::lock_guard lock(mutex_);
  const auto active = active_requests_.find(id);
  if (active == active_requests_.end()) return Invalid("unknown active load");
  if (actual_io_bytes > active->second) return Invalid("completion exceeds reserved I/O bytes");
  active_bytes_ -= active->second;
  active_requests_.erase(active);
  --active_count_;
  SaturatingIncrement(metrics_.completed);
  metrics_.inflight_bytes = active_bytes_;
  return Status::Ok();
}

Status LoadScheduler::Cancel(std::uint64_t id) {
  const auto config_status = Validate();
  if (!config_status.ok()) return config_status;
  std::lock_guard lock(mutex_);
  for (auto& queue : tenants_) {
    const auto found = std::find_if(queue.pending.begin(), queue.pending.end(),
                                    [id](const LoadRequest& request) { return request.id == id; });
    if (found != queue.pending.end()) {
      queue.pending.erase(found);
      --pending_count_;
      SaturatingIncrement(metrics_.cancelled);
      metrics_.pending = pending_count_;
      PruneEmptyQueuesLocked();
      return Status::Ok();
    }
  }
  return Invalid("unknown pending load");
}

SchedulerMetrics LoadScheduler::Metrics() const {
  std::lock_guard lock(mutex_);
  return metrics_;
}

}  // namespace kvstore::kvcache
