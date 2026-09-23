#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

#include "kvstore/common/result.hpp"
#include "kvstore/common/status.hpp"

namespace kvstore {

class IReplicationExecutor {
 public:
  virtual ~IReplicationExecutor() = default;
  [[nodiscard]] virtual Status Submit(std::function<void()> task) = 0;
  [[nodiscard]] virtual Status Execute(std::function<Status()> task) = 0;
  virtual void Shutdown() noexcept = 0;
};

class PthreadReplicationExecutor final : public IReplicationExecutor {
 public:
  explicit PthreadReplicationExecutor(std::size_t queue_capacity);
  ~PthreadReplicationExecutor() override;
  PthreadReplicationExecutor(const PthreadReplicationExecutor&) = delete;
  PthreadReplicationExecutor& operator=(const PthreadReplicationExecutor&) = delete;

  [[nodiscard]] Status Submit(std::function<void()> task) override;
  [[nodiscard]] Status Execute(std::function<Status()> task) override;
  void Shutdown() noexcept override;

 private:
  void Run() noexcept;
  const std::size_t queue_capacity_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::function<void()>> tasks_;
  std::size_t pending_{0};
  bool stopping_{false};
  std::thread worker_;
};

// Adapts an event loop post operation. Posted callbacks retain a queue slot
// until they finish; Shutdown waits for accepted callbacks to drain.
class ReactorReplicationExecutor final : public IReplicationExecutor {
 public:
  using Post = std::function<Status(std::function<void()>)>;
  explicit ReactorReplicationExecutor(Post post, std::size_t queue_capacity = 256);
  ~ReactorReplicationExecutor() override;
  [[nodiscard]] Status Submit(std::function<void()> task) override;
  [[nodiscard]] Status Execute(std::function<Status()> task) override;
  void Shutdown() noexcept override;

 private:
  Post post_;
  const std::size_t queue_capacity_;
  std::mutex mutex_;
  std::condition_variable drained_;
  std::size_t pending_{0};
  bool stopping_{false};
};

// The proactor owns the submitted operation until it invokes completion.
// Completion is mandatory even when execution fails.
class ProactorReplicationExecutor final : public IReplicationExecutor {
 public:
  using Completion = std::function<void()>;
  using SubmitOperation = std::function<Status(std::function<void()>, Completion)>;
  explicit ProactorReplicationExecutor(SubmitOperation submit, std::size_t queue_capacity = 256);
  ~ProactorReplicationExecutor() override;
  [[nodiscard]] Status Submit(std::function<void()> task) override;
  [[nodiscard]] Status Execute(std::function<Status()> task) override;
  void Shutdown() noexcept override;

 private:
  SubmitOperation submit_;
  const std::size_t queue_capacity_;
  std::mutex mutex_;
  std::condition_variable drained_;
  std::size_t pending_{0};
  bool stopping_{false};
};

// Project-owned cooperative single-worker ready queue, not an adapter to a
// third-party ntyco ABI.
class NtycoReplicationExecutor final : public IReplicationExecutor {
 public:
  explicit NtycoReplicationExecutor(std::size_t queue_capacity);
  ~NtycoReplicationExecutor() override;
  [[nodiscard]] Status Submit(std::function<void()> task) override;
  [[nodiscard]] Status Execute(std::function<Status()> task) override;
  void Shutdown() noexcept override;

 private:
  void Run() noexcept;
  const std::size_t queue_capacity_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::function<void()>> tasks_;
  std::size_t pending_{0};
  bool stopping_{false};
  std::thread worker_;
};

struct ReplicationExecutorOptions {
  std::size_t queue_capacity{256};
  ReactorReplicationExecutor::Post reactor_post;
  ProactorReplicationExecutor::SubmitOperation proactor_submit;
};

[[nodiscard]] Result<std::unique_ptr<IReplicationExecutor>> CreateReplicationExecutor(
    std::string_view backend, ReplicationExecutorOptions options = {});

}  // namespace kvstore
