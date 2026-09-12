#include "kvstore/kvcache/request_path.hpp"

#include <future>
#include <limits>
#include <algorithm>

namespace kvstore::kvcache {
struct RequestPath::Pending {
  Pending(TieredStore::Deadline owner_deadline, std::int32_t owner_priority,
          MatchResult owner_match, std::string operation_key)
      : deadline(owner_deadline), priority(owner_priority), match(std::move(owner_match)),
        key(std::move(operation_key)) {}
  std::promise<Result<LookupResult>> promise;
  std::shared_future<Result<LookupResult>> future{promise.get_future().share()};
  std::mutex completion_mutex;
  std::condition_variable completion_cv;
  bool completed{};
  std::size_t waiters{};
  std::uint64_t scheduler_id{};
  std::stop_source stop;
  // These are immutable owner attributes. Coalesced waiters never reprioritize
  // an already submitted scheduler item.
  const TieredStore::Deadline deadline;
  const std::int32_t priority;
  MatchResult match;
  std::string key;
};

RequestPath::RequestPath(MatchIndex& index, TieredStore& store, SchedulerConfig config)
    : index_(index), store_(store), scheduler_(config), shutdown_bound_(config.shutdown_bound),
      worker_paused_for_test_(false) {
  worker_ = std::thread(&RequestPath::Worker, this);
}

RequestPath::~RequestPath() { Shutdown(); }

void RequestPath::Shutdown() noexcept {
  std::lock_guard shutdown_lock(shutdown_mutex_);
  decltype(pending_) abandoned;
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    abandoned.swap(pending_);
  }
  for (const auto& [key, operation] : abandoned) {
    (void)key;
    operation->stop.request_stop();
    (void)scheduler_.Cancel(operation->scheduler_id);
    Complete(operation, Status{StatusCode::kCancelled, "request path stopped"});
  }
  work_cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void RequestPath::Finish(const std::shared_ptr<Pending>& operation, Result<LookupResult> result) {
  {
    std::lock_guard lock(mutex_);
    const auto found = pending_.find(operation->key);
    if (found == pending_.end() || found->second != operation) return;
    pending_.erase(found);
  }
  Complete(operation, std::move(result));
}

void RequestPath::Complete(const std::shared_ptr<Pending>& operation,
                           Result<LookupResult> result) {
  {
    std::lock_guard lock(operation->completion_mutex);
    if (operation->completed) return;
    operation->promise.set_value(std::move(result));
    operation->completed = true;
  }
  operation->completion_cv.notify_all();
}

void RequestPath::Leave(const std::shared_ptr<Pending>& operation) {
  bool last = false;
  {
    std::lock_guard lock(mutex_);
    last = --operation->waiters == 0;
    if (last) {
      // Cancel queued work before a new operation for the same key is admitted.
      (void)scheduler_.Cancel(operation->scheduler_id);
    }
  }
  if (last) {
    operation->stop.request_stop();
    Finish(operation, Status{StatusCode::kCancelled, "lookup has no waiters"});
  }
}

void RequestPath::Worker() {
  for (;;) {
    std::shared_ptr<Pending> operation;
    decltype(pending_) failed;
    Result<LoadTask> task = Status{StatusCode::kInternal, "no scheduled task"};
    {
      std::unique_lock lock(mutex_);
       work_cv_.wait(lock, [&] {
         return stopping_ || (!worker_paused_for_test_ && !pending_.empty());
       });
      if (stopping_) return;
    }
    // Pop may reenter RequestPath through scheduler instrumentation. Keep the
    // request map unlocked while invoking it, then reconcile its result.
    task = scheduler_.Pop(LoadScheduler::Clock::now());
    {
      std::lock_guard lock(mutex_);
      if (!task.ok()) {
        // Pop errors have no task ID. Drain all queued operations so none can
        // remain attached to an unfulfillable future or cause a busy loop.
        failed.swap(pending_);
      } else {
        for (const auto& [key, candidate] : pending_) {
          (void)key;
          if (candidate->scheduler_id == task.value().request.id) {
            operation = candidate;
            break;
          }
        }
      }
    }
    if (!task.ok()) {
      for (const auto& [key, candidate] : failed) {
        (void)key;
        (void)scheduler_.Cancel(candidate->scheduler_id);
        candidate->stop.request_stop();
        Complete(candidate, task.status());
      }
      continue;
    }
    const auto id = task.value().request.id;
    if (!operation) {
      if (task.value().dispatch()) (void)scheduler_.Complete(id, false);
      continue;
    }
    if (!task.value().dispatch()) {
      Finish(operation, Status{task.value().miss_reason == LoadMissReason::kDeadline
                                   ? StatusCode::kDeadlineExceeded : StatusCode::kBusy,
                               "scheduled load cannot dispatch"});
      continue;
    }
    Result<LookupResult> result = Status{StatusCode::kInternal, "load failed"};
    MatchResult match;
    TieredStore::Deadline deadline;
    std::stop_token stop;
    try {
      {
        std::lock_guard lock(mutex_);
        match = operation->match;
        deadline = operation->deadline;
        stop = operation->stop.get_token();
      }
      const auto state = store_.State(match.source_manifest);
      if (!state.ok()) {
        result = state.status();
      } else {
        // The suffix is absent from this source. Load only the matched object;
        // callers recompute the missing tokens using the returned layer range.
        std::vector<TensorRange> load_ranges;
        std::vector<TensorRange> recompute_ranges;
         for (const auto& missing : match.missing_tokens) {
           TensorRange range{missing.begin, missing.end, match.layer_begin,
                             match.layer_begin + match.layer_count};
           if (range.token_end <= match.source_manifest.token_count)
            load_ranges.push_back(range);
          else
            recompute_ranges.push_back(range);
        }
        auto loaded = load_ranges.empty()
                           ? store_.Lookup(match.source_manifest, deadline, stop)
                           : store_.LoadRanges(match.source_manifest, load_ranges, deadline, stop);
        if (!loaded.ok()) {
          result = loaded.status();
        } else {
            result = LookupResult{std::move(match), std::move(loaded.value()),
                                 recompute_ranges.empty() ? MatchKind::kExact : MatchKind::kPrefix,
                                 state.value() == TierState::kResident ? StorageKind::kMemory
                                                                       : StorageKind::kDisk,
                                 std::move(recompute_ranges)};
        }
      }
    } catch (const std::bad_alloc&) {
      result = Status{StatusCode::kLimitExceeded, "lookup allocation failed"};
    } catch (...) {
      result = Status{StatusCode::kInternal, "lookup worker failed"};
    }
    const auto completed = scheduler_.Complete(id, result.ok());
    if (!completed.ok()) result = completed;
    Finish(operation, std::move(result));
  }
}

Result<LookupResult> RequestPath::Lookup(const LookupRequest& request) {
  if (request.cancel.stop_requested()) return Status{StatusCode::kCancelled, "lookup cancelled"};
  if (std::chrono::steady_clock::now() >= request.deadline)
    return Status{StatusCode::kDeadlineExceeded, "lookup deadline exceeded"};
  auto match = request.prefix_lengths.empty()
                   ? index_.Exact(request.query, request.token_ids)
                   : index_.LongestPrefix(request.query, request.token_ids, request.prefix_lengths);
  if (!match.ok()) {
    std::lock_guard lock(mutex_);
    ++metrics_.miss;
    return match.status();
  }
  std::string key = match.value().key.ToString();
  for (const auto& missing : match.value().missing_tokens) {
    key.append("|r:").append(std::to_string(missing.begin)).append(":")
        .append(std::to_string(missing.end)).append(":")
        .append(std::to_string(match.value().layer_begin)).append(":")
        .append(std::to_string(match.value().layer_begin + match.value().layer_count));
  }
  std::shared_ptr<Pending> operation;
  Status submission = Status::Ok();
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return Status{StatusCode::kCancelled, "request path stopped"};
    const auto found = pending_.find(key);
    if (found != pending_.end()) {
      operation = found->second;
      ++metrics_.coalesced;
      test_state_cv_.notify_all();
    } else {
      if (next_id_ == std::numeric_limits<std::uint64_t>::max())
        return Status{StatusCode::kLimitExceeded, "lookup operation IDs exhausted"};
      // A waiter deadline is local. The shared filesystem operation gets its
      // own bounded lifetime so an early waiter cannot cancel later waiters.
      const auto operation_deadline = TieredStore::Deadline::clock::now() + shutdown_bound_;
      operation = std::make_shared<Pending>(operation_deadline,
                                              request.priority, std::move(match.value()), key);
      operation->scheduler_id = ++next_id_;
      pending_.emplace(key, operation);
      test_state_cv_.notify_all();
      LoadRequest task;
      task.id = operation->scheduler_id;
      task.key = key;
      task.tenant = request.query.tenant_id;
      // Deadlines belong to individual waiters, not the first caller's shared load.
      task.deadline = operation->deadline;
      task.priority = operation->priority;
      task.bytes = operation->match.source_manifest.payload_bytes;
      task.estimated_io_bytes = task.bytes;
      const auto submitted = scheduler_.Submit(std::move(task), LoadScheduler::Clock::now());
      if (!submitted.ok()) submission = submitted.status();
      else if (submitted.value() != LoadMissReason::kNone)
        submission = Status{submitted.value() == LoadMissReason::kDeadline
                                ? StatusCode::kDeadlineExceeded : StatusCode::kBusy,
                            "load not schedulable"};
      if (!submission.ok()) pending_.erase(key);
    }
    ++operation->waiters;
  }
  struct Waiter {
    RequestPath& path;
    const std::shared_ptr<Pending>& operation;
    ~Waiter() { path.Leave(operation); }
  } waiter{*this, operation};
  if (!submission.ok()) Complete(operation, submission);
  work_cv_.notify_one();
  std::stop_callback cancellation(request.cancel, [&] {
    // Serialize with the predicate check so cancellation cannot lose a wakeup.
    std::lock_guard lock(operation->completion_mutex);
    operation->completion_cv.notify_all();
  });
  {
    std::unique_lock lock(operation->completion_mutex);
    const auto ready = [&] { return operation->completed || request.cancel.stop_requested(); };
    if (request.deadline == std::chrono::steady_clock::time_point::max())
      operation->completion_cv.wait(lock, ready);
    else
      operation->completion_cv.wait_until(lock, request.deadline, ready);
  }
  Result<LookupResult> result = Status{StatusCode::kCancelled, "lookup cancelled"};
  if (request.cancel.stop_requested()) {
    // The waiter guard cancels the shared operation only when nobody remains.
  } else if (std::chrono::steady_clock::now() >= request.deadline) {
    result = Status{StatusCode::kDeadlineExceeded, "lookup deadline exceeded"};
  } else {
    result = operation->future.get();
  }
  {
    std::lock_guard lock(mutex_);
    if (result.ok()) {
      if (result.value().match_kind == MatchKind::kExact) ++metrics_.exact;
      else ++metrics_.prefix;
      if (result.value().storage_kind == StorageKind::kMemory) ++metrics_.memory;
      else ++metrics_.disk;
    } else {
      ++metrics_.miss;
    }
  }
  return result;
}

LookupMetrics RequestPath::Metrics() const {
  std::lock_guard lock(mutex_);
  return metrics_;
}

SchedulerMetrics RequestPath::SchedulerState() const { return scheduler_.Metrics(); }
}  // namespace kvstore::kvcache
