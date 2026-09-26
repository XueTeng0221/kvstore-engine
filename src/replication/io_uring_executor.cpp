#include "kvstore/replication/executor.hpp"

#ifdef __linux__
#include <linux/io_uring.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <functional>
#include <future>
#include <system_error>

namespace kvstore {
namespace {
Status Error(const char* operation, bool capability = false) {
  const auto code =
      capability && (errno == ENOSYS || errno == EOPNOTSUPP || errno == EPERM || errno == EINVAL)
          ? StatusCode::kUnsupported
          : StatusCode::kIoError;
  return {code, std::system_category().message(errno) + " (" + operation + ")"};
}
}  // namespace

class IoUringReplicationExecutor::Impl {
 public:
  explicit Impl(std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0) {
      status_ = {StatusCode::kInvalidArgument, "io_uring queue capacity must be positive"};
      return;
    }
    io_uring_params params{};
    params.flags = IORING_SETUP_CLAMP;
    constexpr std::size_t kMaximumRingEntries = 32768;
    const auto ring_entries = static_cast<unsigned>(std::min(capacity, kMaximumRingEntries));
    ring_fd_ = static_cast<int>(::syscall(__NR_io_uring_setup, ring_entries, &params));
    if (ring_fd_ < 0) {
      status_ = Error("io_uring_setup", true);
      return;
    }
    event_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE);
    if (event_fd_ < 0) {
      status_ = Error("eventfd");
      Cleanup();
      return;
    }
    const std::size_t sq_size = params.sq_off.array + params.sq_entries * sizeof(unsigned);
    const std::size_t cq_size = params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);
    sq_ring_size_ =
        (params.features & IORING_FEAT_SINGLE_MMAP) ? std::max(sq_size, cq_size) : sq_size;
    cq_ring_size_ = (params.features & IORING_FEAT_SINGLE_MMAP) ? sq_ring_size_ : cq_size;
    sq_ring_ = static_cast<char*>(::mmap(nullptr, sq_ring_size_, PROT_READ | PROT_WRITE,
                                         MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQ_RING));
    if (sq_ring_ == MAP_FAILED) {
      status_ = Error("mmap SQ ring");
      Cleanup();
      return;
    }
    cq_ring_ =
        (params.features & IORING_FEAT_SINGLE_MMAP)
            ? sq_ring_
            : static_cast<char*>(::mmap(nullptr, cq_ring_size_, PROT_READ | PROT_WRITE,
                                        MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_CQ_RING));
    if (cq_ring_ == MAP_FAILED) {
      status_ = Error("mmap CQ ring");
      Cleanup();
      return;
    }
    sq_head_ = reinterpret_cast<unsigned*>(sq_ring_ + params.sq_off.head);
    sq_tail_ = reinterpret_cast<unsigned*>(sq_ring_ + params.sq_off.tail);
    sq_mask_ = reinterpret_cast<unsigned*>(sq_ring_ + params.sq_off.ring_mask);
    sq_entries_ = reinterpret_cast<unsigned*>(sq_ring_ + params.sq_off.ring_entries);
    sq_array_ = reinterpret_cast<unsigned*>(sq_ring_ + params.sq_off.array);
    cq_head_ = reinterpret_cast<unsigned*>(cq_ring_ + params.cq_off.head);
    cq_tail_ = reinterpret_cast<unsigned*>(cq_ring_ + params.cq_off.tail);
    cq_mask_ = reinterpret_cast<unsigned*>(cq_ring_ + params.cq_off.ring_mask);
    cqes_ = reinterpret_cast<io_uring_cqe*>(cq_ring_ + params.cq_off.cqes);
    sqes_ = static_cast<io_uring_sqe*>(::mmap(nullptr, params.sq_entries * sizeof(io_uring_sqe),
                                              PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                                              ring_fd_, IORING_OFF_SQES));
    if (sqes_ == MAP_FAILED) {
      status_ = Error("mmap SQEs");
      Cleanup();
      return;
    }
    worker_ = std::thread([this] { Run(); });
  }
  ~Impl() { Shutdown(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  Status Submit(std::function<void()> task, std::function<void()> failure = {}) {
    if (!task) return {StatusCode::kInvalidArgument, "replication task is empty"};
    std::unique_lock lock(mutex_);
    if (!status_.ok()) return status_;
    if (stopping_) return {StatusCode::kCancelled, "replication executor is stopped"};
    const unsigned tail = std::atomic_ref(*sq_tail_).load(std::memory_order_relaxed);
    const unsigned head = std::atomic_ref(*sq_head_).load(std::memory_order_acquire);
    if (pending_ >= capacity_ || tail - head >= *sq_entries_)
      return {StatusCode::kBusy, "replication task queue is full"};
    auto* context = new Context{std::move(task), std::move(failure)};
    const unsigned index = tail & *sq_mask_;
    io_uring_sqe& sqe = sqes_[index];
    sqe = {};
    sqe.opcode = IORING_OP_READ;
    sqe.fd = event_fd_;
    sqe.addr = reinterpret_cast<std::uintptr_t>(&context->token);
    sqe.len = sizeof(context->token);
    sqe.user_data = reinterpret_cast<std::uintptr_t>(context);
    sq_array_[index] = index;
    std::atomic_ref(*sq_tail_).store(tail + 1U, std::memory_order_release);
    ++pending_;
    int entered = -1;
    do {
      entered = static_cast<int>(::syscall(__NR_io_uring_enter, ring_fd_, 1U, 0U, 0U, nullptr, 0U));
    } while (entered < 0 && errno == EINTR);
    if (entered < 1) {
      status_ = Error("io_uring_enter");
      stopping_ = true;
      auto failure_callback = std::move(context->failure);
      if (!context->accounted.exchange(true, std::memory_order_acq_rel)) {
        --pending_;
      }
      // Keep the published context alive. A syscall error may have happened
      // after the kernel consumed the SQE; reclamation would create a UAF.
      lock.unlock();
      if (failure_callback) failure_callback();
      return status_;
    }
    std::uint64_t token = 1;
    for (;;) {
      if (::write(event_fd_, &token, sizeof(token)) >= 0) break;
      if (errno == EINTR) continue;
      status_ = Error("eventfd write");
      stopping_ = true;
      auto failure_callback = std::move(context->failure);
      if (!context->accounted.exchange(true, std::memory_order_acq_rel)) {
        --pending_;
      }
      lock.unlock();
      if (failure_callback) failure_callback();
      return status_;
    }
    return Status::Ok();
  }
  void Shutdown() noexcept {
    {
      std::scoped_lock lock(mutex_);
      stopping_ = true;
    }
    if (worker_.joinable() && worker_.get_id() == std::this_thread::get_id()) return;
    if (worker_.joinable()) worker_.join();
    Cleanup();
  }

 private:
  struct Context {
    std::function<void()> task;
    std::function<void()> failure;
    std::atomic<bool> accounted{false};
    std::uint64_t token{0};
  };
  void Run() noexcept {
    for (;;) {
      pollfd descriptor{ring_fd_, POLLIN, 0};
      static_cast<void>(::poll(&descriptor, 1, 100));
      for (;;) {
        const unsigned head = std::atomic_ref(*cq_head_).load(std::memory_order_relaxed);
        if (head == std::atomic_ref(*cq_tail_).load(std::memory_order_acquire)) break;
        const io_uring_cqe cqe = cqes_[head & *cq_mask_];
        auto* context = reinterpret_cast<Context*>(cqe.user_data);
        std::function<void()> task;
        std::function<void()> failure;
        bool execute_task = false;
        {
          std::scoped_lock lock(mutex_);
          if (!context->accounted.exchange(true, std::memory_order_acq_rel)) {
            if (cqe.res == static_cast<int>(sizeof(context->token))) {
              task = std::move(context->task);
              execute_task = true;
            } else {
              failure = std::move(context->failure);
            }
            --pending_;
          }
        }
        try {
          if (execute_task)
            task();
          else if (failure)
            failure();
        } catch (...) {
        }
        delete context;
        std::atomic_ref(*cq_head_).store(head + 1U, std::memory_order_release);
      }
      std::scoped_lock lock(mutex_);
      if (stopping_ && pending_ == 0) return;
    }
  }
  void Cleanup() noexcept {
    if (sqes_ != nullptr && sqes_ != MAP_FAILED) {
      ::munmap(sqes_, *sq_entries_ * sizeof(io_uring_sqe));
      sqes_ = nullptr;
    }
    if (cq_ring_ != nullptr && cq_ring_ != MAP_FAILED && cq_ring_ != sq_ring_)
      ::munmap(cq_ring_, cq_ring_size_);
    if (sq_ring_ != nullptr && sq_ring_ != MAP_FAILED) ::munmap(sq_ring_, sq_ring_size_);
    if (ring_fd_ >= 0) ::close(ring_fd_);
    if (event_fd_ >= 0) ::close(event_fd_);
    ring_fd_ = -1;
    event_fd_ = -1;
    sq_ring_ = nullptr;
    cq_ring_ = nullptr;
  }
  const std::size_t capacity_;
  int ring_fd_{-1}, event_fd_{-1};
  std::size_t sq_ring_size_{0}, cq_ring_size_{0};
  char *sq_ring_{nullptr}, *cq_ring_{nullptr};
  io_uring_sqe* sqes_{nullptr};
  io_uring_cqe* cqes_{nullptr};
  unsigned *sq_head_{nullptr}, *sq_tail_{nullptr}, *sq_mask_{nullptr}, *sq_entries_{nullptr}, *sq_array_{nullptr};
  unsigned *cq_head_{nullptr}, *cq_tail_{nullptr}, *cq_mask_{nullptr};
  std::mutex mutex_;
  std::size_t pending_{0};
  bool stopping_{false};
  std::thread worker_;
  Status status_;
};

IoUringReplicationExecutor::IoUringReplicationExecutor(std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)), status_(impl_->status()) {}
IoUringReplicationExecutor::~IoUringReplicationExecutor() { Shutdown(); }
Status IoUringReplicationExecutor::Submit(std::function<void()> task) {
  return impl_->Submit(std::move(task));
}
Status IoUringReplicationExecutor::Execute(std::function<Status()> task) {
  if (!task) return {StatusCode::kInvalidArgument, "replication task is empty"};
  auto promise = std::make_shared<std::promise<Status>>();
  auto future = promise->get_future();
  const auto submitted = impl_->Submit(
      [promise, task = std::move(task)]() mutable {
        try {
          promise->set_value(task());
        } catch (...) {
          promise->set_value({StatusCode::kInternal, "replication task threw"});
        }
      },
      [promise] { promise->set_value({StatusCode::kIoError, "io_uring completion failed"}); });
  return submitted.ok() ? future.get() : submitted;
}
void IoUringReplicationExecutor::Shutdown() noexcept {
  if (impl_) impl_->Shutdown();
}

}  // namespace kvstore
#else
#include <utility>
namespace kvstore {
IoUringReplicationExecutor::IoUringReplicationExecutor(std::size_t)
    : status_(StatusCode::kUnsupported, "io_uring requires Linux") {}
IoUringReplicationExecutor::~IoUringReplicationExecutor() = default;
Status IoUringReplicationExecutor::Submit(std::function<void()>) { return status_; }
Status IoUringReplicationExecutor::Execute(std::function<Status()>) { return status_; }
void IoUringReplicationExecutor::Shutdown() noexcept {}
}  // namespace kvstore
#endif
