#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <functional>
#include <iostream>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>
#include <string_view>
#include <vector>

#include "kvstore/replication/executor.hpp"

namespace {

using Clock = std::chrono::steady_clock;
std::ostream* raw_output = nullptr;

class AsyncPump {
 public:
  AsyncPump() : worker_([this] { Run(); }) {}
  ~AsyncPump() { Stop(); }
  AsyncPump(const AsyncPump&) = delete;
  AsyncPump& operator=(const AsyncPump&) = delete;

  kvstore::Status Post(std::function<void()> task) {
    {
      std::scoped_lock lock(mutex_);
      if (stopping_) return {kvstore::StatusCode::kCancelled, "benchmark pump stopped"};
      tasks_.push_back(std::move(task));
    }
    ready_.notify_one();
    return kvstore::Status::Ok();
  }

  void Stop() {
    {
      std::scoped_lock lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

 private:
  void Run() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        if (tasks_.empty() && stopping_) return;
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task();
    }
  }

  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::function<void()>> tasks_;
  bool stopping_{false};
  std::thread worker_;
};

double Percentile(std::vector<double> values, double percentile) {
  std::sort(values.begin(), values.end());
  const auto index = static_cast<std::size_t>(percentile * static_cast<double>(values.size() - 1U));
  return values[index];
}

int Run(std::string_view backend, std::size_t operations, int round) {
  kvstore::ReplicationExecutorOptions options;
  options.queue_capacity = operations;
  AsyncPump pump;
  options.reactor_post = [&pump](std::function<void()> task) { return pump.Post(std::move(task)); };
  options.proactor_submit = [&pump](std::function<void()> task,
                                    kvstore::ProactorReplicationExecutor::Completion completion) {
    return pump.Post([task = std::move(task), completion = std::move(completion)]() mutable {
      task();
      completion();
    });
  };
  auto executor = kvstore::CreateReplicationExecutor(backend, std::move(options));
  if (!executor.ok()) {
    std::ostringstream unavailable;
    unavailable << "round=" << round << " backend=" << backend
                << " status=unavailable reason=" << executor.status().message() << '\n';
    std::cout << unavailable.str();
    if (raw_output != nullptr) *raw_output << unavailable.str();
    return executor.status().code() == kvstore::StatusCode::kUnsupported ? 0 : 1;
  }

  std::atomic<std::size_t> completed{0};
  const auto warmup_operations = operations / 10U;
  std::atomic<std::size_t> warmup_completed{0};
  for (std::size_t index = 0; index < warmup_operations; ++index) {
    for (;;) {
      const auto status = executor.value()->Submit([&warmup_completed] { warmup_completed.fetch_add(1U); });
      if (status.ok()) break;
      if (status.code() != kvstore::StatusCode::kBusy) return 1;
      std::this_thread::yield();
    }
  }
  while (warmup_completed.load() != warmup_operations) std::this_thread::yield();
  std::vector<double> latency_us(operations, 0.0);
  const auto started = Clock::now();
  for (std::size_t index = 0; index < operations; ++index) {
    const auto task_started = Clock::now();
    for (;;) {
      const auto status = executor.value()->Submit([&completed, &latency_us, index, task_started] {
        latency_us[index] =
            std::chrono::duration<double, std::micro>(Clock::now() - task_started).count();
        completed.fetch_add(1U);
      });
      if (status.ok()) break;
      if (status.code() != kvstore::StatusCode::kBusy) return 1;
      std::this_thread::yield();
    }
  }
  executor.value()->Shutdown();
  pump.Stop();
  if (completed.load() != operations) return 1;
  const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
  std::ostringstream line;
  line << "round=" << round << " backend=" << backend << " operations=" << operations
       << " throughput_ops_sec=" << static_cast<double>(operations) / elapsed
       << " p50_us=" << Percentile(latency_us, 0.50)
       << " p95_us=" << Percentile(latency_us, 0.95)
       << " p99_us=" << Percentile(latency_us, 0.99) << '\n';
  std::cout << line.str();
  if (raw_output != nullptr) *raw_output << line.str();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 2) {
    std::cerr << "Usage: replication_executor_benchmark [raw-result-file]\n";
    return 2;
  }
  constexpr std::size_t kOperations = 100000;
  std::ofstream output;
  if (argc == 2) {
    output.open(argv[1], std::ios::trunc);
    if (!output) return 1;
    raw_output = &output;
  }
  for (int round = 1; round <= 10; ++round) {
    for (const std::string_view backend : {"pthread", "reactor", "io_uring", "ntyco"}) {
      if (Run(backend, kOperations, round) != 0) return 1;
    }
  }
  return 0;
}
