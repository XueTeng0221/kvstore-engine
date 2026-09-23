#include "kvstore/replication/executor.hpp"

#include <atomic>
#include <future>
#include <utility>

namespace kvstore {
namespace {

Status InvalidTask() { return {StatusCode::kInvalidArgument, "replication task is empty"}; }
Status Stopped() { return {StatusCode::kCancelled, "replication executor is stopped"}; }
Status Full() { return {StatusCode::kBusy, "replication task queue is full"}; }

void RunTask(std::function<void()>& task) noexcept {
  try {
    task();
  } catch (...) {
  }
}

Status ExecuteThrough(IReplicationExecutor& executor, std::function<Status()> task) {
  if (!task) return InvalidTask();
  auto result = std::make_shared<std::promise<Status>>();
  auto future = result->get_future();
  const auto submitted = executor.Submit([result, task = std::move(task)]() mutable {
    try {
      result->set_value(task());
    } catch (...) {
      result->set_value({StatusCode::kInternal, "replication task threw"});
    }
  });
  if (!submitted.ok()) return submitted;
  return future.get();
}

}  // namespace

PthreadReplicationExecutor::PthreadReplicationExecutor(std::size_t queue_capacity)
    : queue_capacity_(queue_capacity), worker_(&PthreadReplicationExecutor::Run, this) {}
PthreadReplicationExecutor::~PthreadReplicationExecutor() { Shutdown(); }

Status PthreadReplicationExecutor::Submit(std::function<void()> task) {
  if (!task) return InvalidTask();
  std::scoped_lock lock(mutex_);
  if (stopping_) return Stopped();
  if (queue_capacity_ == 0 || pending_ >= queue_capacity_) return Full();
  tasks_.push_back(std::move(task));
  ++pending_;
  ready_.notify_one();
  return Status::Ok();
}

Status PthreadReplicationExecutor::Execute(std::function<Status()> task) {
  return ExecuteThrough(*this, std::move(task));
}

void PthreadReplicationExecutor::Shutdown() noexcept {
  {
    std::scoped_lock lock(mutex_);
    stopping_ = true;
  }
  ready_.notify_all();
  if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
}

void PthreadReplicationExecutor::Run() noexcept {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock lock(mutex_);
      ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      if (tasks_.empty() && stopping_) return;
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    RunTask(task);
    {
      std::scoped_lock lock(mutex_);
      --pending_;
    }
  }
}

ReactorReplicationExecutor::ReactorReplicationExecutor(Post post, std::size_t capacity)
    : post_(std::move(post)), queue_capacity_(capacity) {}
ReactorReplicationExecutor::~ReactorReplicationExecutor() { Shutdown(); }

Status ReactorReplicationExecutor::Submit(std::function<void()> task) {
  if (!task) return InvalidTask();
  {
    std::scoped_lock lock(mutex_);
    if (stopping_) return Stopped();
    if (!post_) return {StatusCode::kUnsupported, "reactor post function is unavailable"};
    if (pending_ >= queue_capacity_) return Full();
    ++pending_;
  }
  auto completed = std::make_shared<std::atomic<bool>>(false);
  Status posted;
  try {
    posted = post_([this, completed, task = std::move(task)]() mutable {
      RunTask(task);
      if (completed->exchange(true, std::memory_order_acq_rel)) return;
      {
        std::scoped_lock lock(mutex_);
        --pending_;
      }
      drained_.notify_all();
    });
  } catch (...) {
    posted = {StatusCode::kInternal, "reactor post callback threw"};
  }
  if (!posted.ok()) {
    if (completed->exchange(true, std::memory_order_acq_rel)) return posted;
    {
      std::scoped_lock lock(mutex_);
      --pending_;
    }
    drained_.notify_all();
  }
  return posted;
}

Status ReactorReplicationExecutor::Execute(std::function<Status()> task) {
  if (!task) return InvalidTask();
  {
    std::scoped_lock lock(mutex_);
    if (stopping_) return Stopped();
  }
  try {
    return task();
  } catch (...) {
    return {StatusCode::kInternal, "replication task threw"};
  }
}

void ReactorReplicationExecutor::Shutdown() noexcept {
  std::unique_lock lock(mutex_);
  stopping_ = true;
  drained_.wait(lock, [this] { return pending_ == 0; });
}

ProactorReplicationExecutor::ProactorReplicationExecutor(SubmitOperation submit,
                                                         std::size_t capacity)
    : submit_(std::move(submit)), queue_capacity_(capacity) {}
ProactorReplicationExecutor::~ProactorReplicationExecutor() { Shutdown(); }

Status ProactorReplicationExecutor::Submit(std::function<void()> task) {
  if (!task) return InvalidTask();
  {
    std::scoped_lock lock(mutex_);
    if (stopping_) return Stopped();
    if (!submit_) return {StatusCode::kUnsupported, "proactor submit function is unavailable"};
    if (pending_ >= queue_capacity_) return Full();
    ++pending_;
  }
  auto completed = std::make_shared<std::atomic<bool>>(false);
  Completion completion = [this, completed] {
    if (completed->exchange(true, std::memory_order_acq_rel)) return;
    {
      std::scoped_lock lock(mutex_);
      --pending_;
    }
    drained_.notify_all();
  };
  Status submitted;
  try {
    submitted = submit_([task = std::move(task)]() mutable { RunTask(task); }, completion);
  } catch (...) {
    submitted = {StatusCode::kInternal, "proactor submit callback threw"};
  }
  if (!submitted.ok()) completion();
  return submitted;
}

Status ProactorReplicationExecutor::Execute(std::function<Status()> task) {
  return ExecuteThrough(*this, std::move(task));
}

void ProactorReplicationExecutor::Shutdown() noexcept {
  std::unique_lock lock(mutex_);
  stopping_ = true;
  drained_.wait(lock, [this] { return pending_ == 0; });
}

NtycoReplicationExecutor::NtycoReplicationExecutor(std::size_t capacity)
    : queue_capacity_(capacity), worker_(&NtycoReplicationExecutor::Run, this) {}
NtycoReplicationExecutor::~NtycoReplicationExecutor() { Shutdown(); }

Status NtycoReplicationExecutor::Submit(std::function<void()> task) {
  if (!task) return InvalidTask();
  std::scoped_lock lock(mutex_);
  if (stopping_) return Stopped();
  if (queue_capacity_ == 0 || pending_ >= queue_capacity_) return Full();
  tasks_.push_back(std::move(task));
  ++pending_;
  ready_.notify_one();
  return Status::Ok();
}

Status NtycoReplicationExecutor::Execute(std::function<Status()> task) {
  return ExecuteThrough(*this, std::move(task));
}

void NtycoReplicationExecutor::Shutdown() noexcept {
  {
    std::scoped_lock lock(mutex_);
    stopping_ = true;
  }
  ready_.notify_all();
  if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
}

void NtycoReplicationExecutor::Run() noexcept {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock lock(mutex_);
      ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      if (tasks_.empty() && stopping_) return;
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    RunTask(task);
    {
      std::scoped_lock lock(mutex_);
      --pending_;
    }
  }
}

Result<std::unique_ptr<IReplicationExecutor>> CreateReplicationExecutor(
    std::string_view backend, ReplicationExecutorOptions options) {
  if (backend == "pthread")
    return std::unique_ptr<IReplicationExecutor>(
        std::make_unique<PthreadReplicationExecutor>(options.queue_capacity));
  if (backend == "reactor") {
    if (!options.reactor_post)
      return Status{StatusCode::kInvalidArgument, "reactor backend requires a post callback"};
    return std::unique_ptr<IReplicationExecutor>(std::make_unique<ReactorReplicationExecutor>(
        std::move(options.reactor_post), options.queue_capacity));
  }
  if (backend == "proactor") {
    if (!options.proactor_submit)
      return Status{StatusCode::kInvalidArgument, "proactor backend requires a submit callback"};
    return std::unique_ptr<IReplicationExecutor>(std::make_unique<ProactorReplicationExecutor>(
        std::move(options.proactor_submit), options.queue_capacity));
  }
  if (backend == "ntyco")
    return std::unique_ptr<IReplicationExecutor>(
        std::make_unique<NtycoReplicationExecutor>(options.queue_capacity));
  return Status{StatusCode::kInvalidArgument, "unknown replication executor backend"};
}

}  // namespace kvstore
