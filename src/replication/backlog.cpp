#include "kvstore/replication/backlog.hpp"

#include <limits>

namespace kvstore {

Status ReplicationBacklog::Append(const std::vector<WriteEvent>& events) {
  if (events.empty()) return Status::Ok();
  if (events.size() > kCapacity)
    return {StatusCode::kLimitExceeded, "replication backlog batch exceeds slot capacity"};
  if (events.front().offset == 0 ||
      events.front().offset == std::numeric_limits<std::uint64_t>::max())
    return {StatusCode::kInvalidArgument, "backlog offset is invalid"};
  if (events.size() > 1U) {
    for (std::size_t index = 1; index < events.size(); ++index) {
      if (events[index - 1U].offset == std::numeric_limits<std::uint64_t>::max() ||
          events[index].offset != events[index - 1U].offset + 1U)
        return {StatusCode::kInvalidArgument, "backlog batch offsets are not contiguous"};
      if (events[index - 1U].event_id == std::numeric_limits<std::uint64_t>::max() ||
          events[index].event_id != events[index - 1U].event_id + 1U)
        return {StatusCode::kInvalidArgument, "backlog batch event ids are not contiguous"};
    }
  }
  for (const auto& event : events) {
    if (event.source != WriteSource::kClient || event.ComputeChecksum() != event.checksum)
      return {StatusCode::kInvalidArgument, "backlog event metadata is invalid"};
  }
  std::scoped_lock lock(mutex_);
  const auto current_newest =
      size_ == 0 ? 0 : slots_[(next_slot_ + kCapacity - 1U) % kCapacity].last_offset;
  if (current_newest != 0 && events.front().offset != current_newest + 1U)
    return {StatusCode::kInvalidArgument, "backlog offset is not monotonic"};
  if (size_ != 0) {
    const auto newest_id = slots_[(next_slot_ + kCapacity - 1U) % kCapacity].events.back().event_id;
    if (newest_id == std::numeric_limits<std::uint64_t>::max() ||
        events.front().event_id != newest_id + 1U)
      return {StatusCode::kInvalidArgument, "backlog event id is not monotonic"};
  }
  for (const auto& event : events) {
    auto& slot = slots_[next_slot_];
    slot = Batch{event.offset, event.offset, {event}};
    next_slot_ = (next_slot_ + 1U) % kCapacity;
    if (size_ < kCapacity) ++size_;
  }
  return Status::Ok();
}

Result<std::vector<WriteEvent>> ReplicationBacklog::ReadFrom(std::uint64_t next_offset) const {
  std::scoped_lock lock(mutex_);
  if (next_offset == 0) return Status{StatusCode::kInvalidArgument, "invalid replication offset"};
  const auto newest =
      size_ == 0 ? 0 : slots_[(next_slot_ + kCapacity - 1U) % kCapacity].last_offset;
  const auto oldest =
      size_ == 0 ? 0 : slots_[(next_slot_ + kCapacity - size_) % kCapacity].first_offset;
  if (size_ == 0 || next_offset > newest) return std::vector<WriteEvent>{};
  if (next_offset < oldest)
    return Status{StatusCode::kBusy, "replication backlog no longer covers offset"};
  std::vector<WriteEvent> result;
  result.reserve(newest - next_offset + 1U);
  for (std::size_t index = 0; index < size_; ++index) {
    const auto slot_index = (next_slot_ + kCapacity - size_ + index) % kCapacity;
    const auto& slot = slots_[slot_index];
    if (slot.first_offset >= next_offset)
      result.insert(result.end(), slot.events.begin(), slot.events.end());
  }
  if (result.empty() || result.front().offset != next_offset)
    return Status{StatusCode::kCorruption, "replication backlog has a gap"};
  return result;
}

Result<WriteEvent> ReplicationBacklog::ReadOne(std::uint64_t offset) const {
  std::scoped_lock lock(mutex_);
  if (offset == 0) return Status{StatusCode::kInvalidArgument, "invalid replication offset"};
  if (size_ == 0) return Status{StatusCode::kNotFound, "replication offset is not present"};
  const auto oldest = slots_[(next_slot_ + kCapacity - size_) % kCapacity].first_offset;
  const auto newest = slots_[(next_slot_ + kCapacity - 1U) % kCapacity].last_offset;
  if (offset < oldest)
    return Status{StatusCode::kBusy, "replication backlog no longer covers offset"};
  if (offset > newest) return Status{StatusCode::kNotFound, "replication offset is not present"};
  for (std::size_t index = 0; index < size_; ++index) {
    const auto slot_index = (next_slot_ + kCapacity - size_ + index) % kCapacity;
    const auto& slot = slots_[slot_index];
    if (slot.first_offset == offset && !slot.events.empty()) return slot.events.front();
  }
  return Status{StatusCode::kCorruption, "replication backlog has a gap"};
}

std::uint64_t ReplicationBacklog::newest_offset() const {
  std::scoped_lock lock(mutex_);
  if (size_ == 0) return 0;
  return slots_[(next_slot_ + kCapacity - 1U) % kCapacity].last_offset;
}

std::uint64_t ReplicationBacklog::newest_event_id() const {
  std::scoped_lock lock(mutex_);
  if (size_ == 0) return 0;
  return slots_[(next_slot_ + kCapacity - 1U) % kCapacity].events.back().event_id;
}

std::pair<std::uint64_t, std::uint64_t> ReplicationBacklog::newest_point() const {
  std::scoped_lock lock(mutex_);
  if (size_ == 0) return {0, 0};
  const auto& slot = slots_[(next_slot_ + kCapacity - 1U) % kCapacity];
  return {slot.last_offset, slot.events.back().event_id};
}

std::uint64_t ReplicationBacklog::oldest_offset() const {
  std::scoped_lock lock(mutex_);
  if (size_ == 0) return 0;
  return slots_[(next_slot_ + kCapacity - size_) % kCapacity].first_offset;
}

std::size_t ReplicationBacklog::size() const {
  std::scoped_lock lock(mutex_);
  return size_;
}

void ReplicationBacklog::Reset() {
  std::scoped_lock lock(mutex_);
  for (auto& slot : slots_) slot = {};
  size_ = 0;
  next_slot_ = 0;
}

}  // namespace kvstore
