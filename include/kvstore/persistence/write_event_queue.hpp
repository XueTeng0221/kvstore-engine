#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <future>
#include <mutex>
#include <vector>

#include "kvstore/command/dispatcher.hpp"
#include "kvstore/common/resources.hpp"

namespace kvstore {

class WriteEventQueue {
 public:
  WriteEventQueue(std::size_t record_capacity, std::size_t byte_capacity, BatchEventSink consumer);
  ~WriteEventQueue();
  WriteEventQueue(const WriteEventQueue&) = delete;
  WriteEventQueue& operator=(const WriteEventQueue&) = delete;

  [[nodiscard]] Status Submit(const WriteEvent& event);
  [[nodiscard]] Status Submit(const WriteEvent& event,
                              std::chrono::steady_clock::time_point deadline,
                              std::stop_token stop = {});
  [[nodiscard]] Status SubmitBatch(const std::vector<WriteEvent>& events);
  [[nodiscard]] Status SubmitBatch(const std::vector<WriteEvent>& events,
                                   std::chrono::steady_clock::time_point deadline,
                                   std::stop_token stop = {});
  void Stop() noexcept;

 private:
  struct Item {
    std::vector<WriteEvent> events;
    std::size_t bytes{};
    std::promise<Status> result;
  };

  [[nodiscard]] static std::size_t Bytes(const std::vector<WriteEvent>& events);
  void Consume(std::stop_token stop);

  std::size_t record_capacity_;
  std::size_t byte_capacity_;
  BatchEventSink consumer_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Item> queue_;
  std::size_t queued_records_{0};
  std::size_t queued_bytes_{0};
  bool stopping_{false};
  bool failed_{false};
  OwnedThread worker_;
};

}  // namespace kvstore
