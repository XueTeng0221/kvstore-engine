#include "kvstore/engine/hash_engine.hpp"

#include <algorithm>
#include <mutex>

#include "engine_util.hpp"

namespace kvstore {

HashEngine::HashEngine(std::size_t capacity, std::size_t initial_buckets, float max_load_factor)
    : capacity_(capacity) {
  entries_.max_load_factor(max_load_factor);
  entries_.rehash(initial_buckets);
}

Status HashEngine::Create(std::string_view key, std::string_view value, std::stop_token stop) {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::unique_lock lock(mutex_);
  if (entries_.contains(std::string(key)))
    return {StatusCode::kAlreadyExists, "key already exists"};
  if (entries_.size() >= capacity_) return {StatusCode::kLimitExceeded, "engine capacity reached"};
  if (stop.stop_requested()) return {StatusCode::kCancelled, "operation cancelled"};
  entries_.emplace(key, value);
  return Status::Ok();
}

Result<std::string> HashEngine::Get(std::string_view key, std::stop_token stop) const {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::shared_lock lock(mutex_);
  const auto entry = entries_.find(std::string(key));
  if (entry == entries_.end()) return Status{StatusCode::kNotFound, "key not found"};
  return entry->second;
}

Status HashEngine::Delete(std::string_view key, std::stop_token stop) {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::unique_lock lock(mutex_);
  const auto entry = entries_.find(std::string(key));
  if (entry == entries_.end()) return {StatusCode::kNotFound, "key not found"};
  if (stop.stop_requested()) return {StatusCode::kCancelled, "operation cancelled"};
  entries_.erase(entry);
  return Status::Ok();
}

Status HashEngine::Modify(std::string_view key, std::string_view value, std::stop_token stop) {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::unique_lock lock(mutex_);
  const auto entry = entries_.find(std::string(key));
  if (entry == entries_.end()) return {StatusCode::kNotFound, "key not found"};
  if (stop.stop_requested()) return {StatusCode::kCancelled, "operation cancelled"};
  entry->second.assign(value);
  return Status::Ok();
}

Result<bool> HashEngine::Upsert(std::string_view key, std::string_view value,
                                std::stop_token stop) {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::unique_lock lock(mutex_);
  const auto entry = entries_.find(std::string(key));
  if (entry != entries_.end()) {
    if (stop.stop_requested()) return Status{StatusCode::kCancelled, "operation cancelled"};
    entry->second.assign(value);
    return false;
  }
  if (entries_.size() >= capacity_)
    return Status{StatusCode::kLimitExceeded, "engine capacity reached"};
  if (stop.stop_requested()) return Status{StatusCode::kCancelled, "operation cancelled"};
  entries_.emplace(key, value);
  return true;
}

Result<bool> HashEngine::Exists(std::string_view key, std::stop_token stop) const {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::shared_lock lock(mutex_);
  return entries_.contains(std::string(key));
}

Result<std::int64_t> HashEngine::Increment(std::string_view key, std::int64_t delta,
                                           std::stop_token stop) {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::unique_lock lock(mutex_);
  const auto entry = entries_.find(std::string(key));
  std::int64_t current = 0;
  if (entry != entries_.end()) {
    auto parsed = engine_internal::ParseInteger(entry->second);
    if (!parsed.ok()) return parsed.status();
    current = parsed.value();
  } else if (entries_.size() >= capacity_) {
    return Status{StatusCode::kLimitExceeded, "engine capacity reached"};
  }
  auto updated = engine_internal::AddChecked(current, delta);
  if (!updated.ok()) return updated.status();
  if (stop.stop_requested()) return Status{StatusCode::kCancelled, "operation cancelled"};
  if (entry == entries_.end()) {
    entries_.emplace(key, engine_internal::FormatInteger(updated.value()));
  } else {
    entry->second = engine_internal::FormatInteger(updated.value());
  }
  return updated.value();
}

Result<std::vector<Entry>> HashEngine::Scan(std::stop_token stop) const {
  auto status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::shared_lock lock(mutex_);
  std::vector<Entry> output;
  output.reserve(entries_.size());
  for (const auto& [key, value] : entries_) output.push_back({key, value});
  std::sort(output.begin(), output.end(),
            [](const Entry& left, const Entry& right) { return left.key < right.key; });
  return output;
}

Result<std::vector<Entry>> HashEngine::Export(std::stop_token stop) const { return Scan(stop); }

Status HashEngine::Import(const std::vector<Entry>& entries, std::stop_token stop) {
  auto status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  if (entries.size() > capacity_) return {StatusCode::kLimitExceeded, "import exceeds capacity"};
  std::unordered_map<std::string, std::string> replacement;
  replacement.max_load_factor(entries_.max_load_factor());
  replacement.reserve(entries.size());
  for (const auto& entry : entries) {
    status = engine_internal::ValidateKey(entry.key);
    if (!status.ok()) return status;
    if (!replacement.emplace(entry.key, entry.value).second) {
      return {StatusCode::kAlreadyExists, "duplicate import key"};
    }
  }
  std::unique_lock lock(mutex_);
  if (stop.stop_requested()) return {StatusCode::kCancelled, "operation cancelled"};
  entries_.swap(replacement);
  return Status::Ok();
}

std::size_t HashEngine::Size() const {
  std::shared_lock lock(mutex_);
  return entries_.size();
}

}  // namespace kvstore
