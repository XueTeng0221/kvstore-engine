#include "kvstore/persistence/write_event_queue.hpp"

#include <limits>

namespace kvstore {

WriteEventQueue::WriteEventQueue(std::size_t record_capacity, std::size_t byte_capacity,
                                 BatchEventSink consumer)
    : record_capacity_(record_capacity),
      byte_capacity_(byte_capacity),
      consumer_(std::move(consumer)),
      worker_([this](std::stop_token stop) { Consume(stop); }) {}

WriteEventQueue::~WriteEventQueue() { Stop(); }

std::size_t WriteEventQueue::Bytes(const std::vector<WriteEvent>& events) {
  std::size_t total = 0;
  for (const auto& event : events) {
    const auto payload = event.key.size() + event.value.size() + event.origin_node.size() + 64U;
    if (payload > std::numeric_limits<std::size_t>::max() - total)
      return std::numeric_limits<std::size_t>::max();
    total += payload;
  }
  return total;
}

Status WriteEventQueue::Submit(const WriteEvent& event) {
  return SubmitBatch(std::vector<WriteEvent>{event});
}

Status WriteEventQueue::Submit(const WriteEvent& event,
                               std::chrono::steady_clock::time_point deadline,
                               std::stop_token stop) {
  return SubmitBatch(std::vector<WriteEvent>{event}, deadline, stop);
}

Status WriteEventQueue::SubmitBatch(const std::vector<WriteEvent>& events) {
  return SubmitBatch(events, std::chrono::steady_clock::time_point::max());
}

Status WriteEventQueue::SubmitBatch(const std::vector<WriteEvent>& events,
                                    std::chrono::steady_clock::time_point deadline,
                                    std::stop_token stop) {
  if (events.empty()) return Status::Ok();
  const auto bytes = Bytes(events);
  if (events.size() > record_capacity_ || bytes > byte_capacity_)
    return {StatusCode::kLimitExceeded, "write event exceeds queue capacity"};
  Item item{events, bytes, {}};
  auto result = item.result.get_future();
  {
    std::unique_lock lock(mutex_);
    while (!stopping_ && !failed_ && !stop.stop_requested() &&
           (events.size() > record_capacity_ - queued_records_ ||
            bytes > byte_capacity_ - queued_bytes_)) {
      if (condition_.wait_until(lock, deadline) == std::cv_status::timeout)
        return {StatusCode::kBusy, "write event queue deadline exceeded"};
    }
    if (stop.stop_requested()) return {StatusCode::kCancelled, "write event queue cancelled"};
    if (failed_) return {StatusCode::kIoError, "write event queue is failed"};
    if (stopping_) return {StatusCode::kCancelled, "write event queue is stopping"};
    queued_records_ += events.size();
    queued_bytes_ += bytes;
    queue_.push_back(std::move(item));
  }
  condition_.notify_all();
  while (result.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
    if (stop.stop_requested()) {
      std::scoped_lock lock(mutex_);
      failed_ = true;
      return {StatusCode::kInternal, "write event completion is indeterminate after cancellation"};
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      std::scoped_lock lock(mutex_);
      failed_ = true;
      return {StatusCode::kInternal, "write event completion exceeded deadline"};
    }
  }
  return result.get();
}

void WriteEventQueue::Stop() noexcept {
  {
    std::scoped_lock lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
  }
  condition_.notify_all();
  worker_.request_stop();
  if (worker_.joinable()) worker_.join();
}

void WriteEventQueue::Consume(std::stop_token stop) {
  while (true) {
    Item item;
    {
      std::unique_lock lock(mutex_);
      condition_.wait(
          lock, [this, &stop] { return stopping_ || stop.stop_requested() || !queue_.empty(); });
      if (queue_.empty()) {
        if (stopping_ || stop.stop_requested()) return;
        continue;
      }
      item = std::move(queue_.front());
      queue_.pop_front();
      queued_records_ -= item.events.size();
      queued_bytes_ -= item.bytes;
    }
    condition_.notify_all();
    item.result.set_value(consumer_ ? consumer_(item.events) : Status::Ok());
  }
}

}  // namespace kvstore
