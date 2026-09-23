#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include "kvstore/command/dispatcher.hpp"

namespace kvstore {

class LiveSyncDeduplicator {
 public:
  using Clock = std::chrono::steady_clock;
  enum class Observation { kNew, kDuplicate, kConflict };

  LiveSyncDeduplicator(std::size_t capacity, std::chrono::milliseconds ttl);

  // Returns true once per origin/event pair during the retention window.
  [[nodiscard]] Observation Observe(const WriteEvent& event, Clock::time_point now);
  void Forget(const WriteEvent& event);
  [[nodiscard]] std::size_t size() const;

 private:
  struct Seen {
    std::string key;
    Clock::time_point expires;
  };
  struct Record {
    Clock::time_point expires;
    std::uint32_t checksum{};
  };

  void Expire(Clock::time_point now);
  [[nodiscard]] static std::string Key(const WriteEvent& event);

  const std::size_t capacity_;
  const std::chrono::milliseconds ttl_;
  mutable std::mutex mutex_;
  std::deque<Seen> order_;
  std::unordered_map<std::string, Record> seen_;
};

}  // namespace kvstore
