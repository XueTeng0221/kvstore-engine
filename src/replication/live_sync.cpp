#include "kvstore/replication/live_sync.hpp"

namespace kvstore {

LiveSyncDeduplicator::LiveSyncDeduplicator(std::size_t capacity, std::chrono::milliseconds ttl)
    : capacity_(capacity), ttl_(ttl) {}

void LiveSyncDeduplicator::Expire(Clock::time_point now) {
  while (!order_.empty() && order_.front().expires <= now) {
    const auto found = seen_.find(order_.front().key);
    if (found != seen_.end() && found->second.expires == order_.front().expires) seen_.erase(found);
    order_.pop_front();
  }
}

LiveSyncDeduplicator::Observation LiveSyncDeduplicator::Observe(const WriteEvent& event,
                                                                Clock::time_point now) {
  if (event.origin_node.empty() || event.event_id == 0 || capacity_ == 0 || ttl_.count() <= 0)
    return Observation::kConflict;

  auto key = Key(event);

  std::scoped_lock lock(mutex_);
  Expire(now);
  if (const auto found = seen_.find(key); found != seen_.end())
    return found->second.checksum == event.checksum ? Observation::kDuplicate
                                                    : Observation::kConflict;
  while (seen_.size() >= capacity_ && !order_.empty()) {
    const auto found = seen_.find(order_.front().key);
    if (found != seen_.end() && found->second.expires == order_.front().expires) seen_.erase(found);
    order_.pop_front();
  }
  if (now > Clock::time_point::max() - ttl_) return Observation::kConflict;
  const auto expires = now + ttl_;
  seen_.emplace(key, Record{expires, event.checksum});
  order_.push_back({std::move(key), expires});
  return Observation::kNew;
}

std::string LiveSyncDeduplicator::Key(const WriteEvent& event) {
  std::string key;
  key.reserve(event.origin_node.size() + 24U);
  key.append(std::to_string(event.origin_node.size())).push_back(':');
  key.append(event.origin_node).push_back(':');
  key.append(std::to_string(event.event_id));
  return key;
}

void LiveSyncDeduplicator::Forget(const WriteEvent& event) {
  if (event.origin_node.empty() || event.event_id == 0) return;
  std::scoped_lock lock(mutex_);
  seen_.erase(Key(event));
}

std::size_t LiveSyncDeduplicator::size() const {
  std::scoped_lock lock(mutex_);
  return seen_.size();
}

}  // namespace kvstore
