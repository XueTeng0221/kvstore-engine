#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include "kvstore/command/dispatcher.hpp"

namespace kvstore {

class ReplicationBacklog {
 public:
  static constexpr std::size_t kCapacity = 1024;
  struct Batch {
    std::uint64_t first_offset{};
    std::uint64_t last_offset{};
    std::vector<WriteEvent> events;
  };

  [[nodiscard]] Status Append(const std::vector<WriteEvent>& events);
  [[nodiscard]] Result<std::vector<WriteEvent>> ReadFrom(std::uint64_t next_offset) const;
  [[nodiscard]] Result<WriteEvent> ReadOne(std::uint64_t offset) const;
  [[nodiscard]] std::uint64_t newest_offset() const;
  [[nodiscard]] std::uint64_t newest_event_id() const;
  [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> newest_point() const;
  [[nodiscard]] std::uint64_t oldest_offset() const;
  [[nodiscard]] std::size_t size() const;
  void Reset();

 private:
  mutable std::mutex mutex_;
  std::array<Batch, kCapacity> slots_{};
  std::size_t size_{0};
  std::size_t next_slot_{0};
};

}  // namespace kvstore
