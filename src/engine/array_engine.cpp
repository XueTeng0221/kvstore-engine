#include "kvstore/engine/array_engine.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_set>

#include "engine_util.hpp"

namespace kvstore {

ArrayEngine::ArrayEngine(std::size_t capacity, std::size_t reserve) : capacity_(capacity) {
  entries_.reserve(std::min(capacity, reserve));
}

ArrayEngine::Iterator ArrayEngine::Find(std::string_view key) {
  return std::find_if(entries_.begin(), entries_.end(),
                      [key](const Entry& entry) { return entry.key == key; });
}

ArrayEngine::ConstIterator ArrayEngine::Find(std::string_view key) const {
  return std::find_if(entries_.begin(), entries_.end(),
                      [key](const Entry& entry) { return entry.key == key; });
}

Status ArrayEngine::Create(std::string_view key, std::string_view value, std::stop_token stop) {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::unique_lock lock(mutex_);
  if (Find(key) != entries_.end()) return {StatusCode::kAlreadyExists, "key already exists"};
  if (entries_.size() >= capacity_) return {StatusCode::kLimitExceeded, "engine capacity reached"};
  if (stop.stop_requested()) return {StatusCode::kCancelled, "operation cancelled"};
  entries_.push_back({std::string(key), std::string(value)});
  return Status::Ok();
}

Result<std::string> ArrayEngine::Get(std::string_view key, std::stop_token stop) const {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::shared_lock lock(mutex_);
  const auto entry = Find(key);
  if (entry == entries_.end()) return Status{StatusCode::kNotFound, "key not found"};
  return entry->value;
}

Status ArrayEngine::Delete(std::string_view key, std::stop_token stop) {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::unique_lock lock(mutex_);
  const auto entry = Find(key);
  if (entry == entries_.end()) return {StatusCode::kNotFound, "key not found"};
  if (stop.stop_requested()) return {StatusCode::kCancelled, "operation cancelled"};
  if (entry != entries_.end() - 1) *entry = std::move(entries_.back());
  entries_.pop_back();
  return Status::Ok();
}

Status ArrayEngine::Modify(std::string_view key, std::string_view value, std::stop_token stop) {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::unique_lock lock(mutex_);
  const auto entry = Find(key);
  if (entry == entries_.end()) return {StatusCode::kNotFound, "key not found"};
  if (stop.stop_requested()) return {StatusCode::kCancelled, "operation cancelled"};
  entry->value.assign(value);
  return Status::Ok();
}

Result<bool> ArrayEngine::Upsert(std::string_view key, std::string_view value,
                                 std::stop_token stop) {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::unique_lock lock(mutex_);
  const auto entry = Find(key);
  if (entry != entries_.end()) {
    if (stop.stop_requested()) return Status{StatusCode::kCancelled, "operation cancelled"};
    entry->value.assign(value);
    return false;
  }
  if (entries_.size() >= capacity_)
    return Status{StatusCode::kLimitExceeded, "engine capacity reached"};
  if (stop.stop_requested()) return Status{StatusCode::kCancelled, "operation cancelled"};
  entries_.push_back({std::string(key), std::string(value)});
  return true;
}

Result<bool> ArrayEngine::Exists(std::string_view key, std::stop_token stop) const {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::shared_lock lock(mutex_);
  return Find(key) != entries_.end();
}

Result<std::int64_t> ArrayEngine::Increment(std::string_view key, std::int64_t delta,
                                            std::stop_token stop) {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::unique_lock lock(mutex_);
  const auto entry = Find(key);
  std::int64_t current = 0;
  if (entry != entries_.end()) {
    auto parsed = engine_internal::ParseInteger(entry->value);
    if (!parsed.ok()) return parsed.status();
    current = parsed.value();
  } else if (entries_.size() >= capacity_) {
    return Status{StatusCode::kLimitExceeded, "engine capacity reached"};
  }
  auto updated = engine_internal::AddChecked(current, delta);
  if (!updated.ok()) return updated.status();
  if (stop.stop_requested()) return Status{StatusCode::kCancelled, "operation cancelled"};
  if (entry == entries_.end()) {
    entries_.push_back({std::string(key), engine_internal::FormatInteger(updated.value())});
  } else {
    entry->value = engine_internal::FormatInteger(updated.value());
  }
  return updated.value();
}

Result<std::vector<Entry>> ArrayEngine::Scan(std::stop_token stop) const {
  auto status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::shared_lock lock(mutex_);
  auto output = entries_;
  std::sort(output.begin(), output.end(),
            [](const Entry& left, const Entry& right) { return left.key < right.key; });
  return output;
}

Result<std::vector<Entry>> ArrayEngine::Export(std::stop_token stop) const { return Scan(stop); }

Status ArrayEngine::Import(const std::vector<Entry>& entries, std::stop_token stop) {
  auto status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  if (entries.size() > capacity_) return {StatusCode::kLimitExceeded, "import exceeds capacity"};
  std::unordered_set<std::string> keys;
  std::vector<Entry> replacement;
  replacement.reserve(entries.size());
  for (const auto& entry : entries) {
    status = engine_internal::ValidateKey(entry.key);
    if (!status.ok()) return status;
    if (!keys.insert(entry.key).second) return {StatusCode::kAlreadyExists, "duplicate import key"};
    replacement.push_back(entry);
  }
  std::unique_lock lock(mutex_);
  if (stop.stop_requested()) return {StatusCode::kCancelled, "operation cancelled"};
  entries_.swap(replacement);
  return Status::Ok();
}

std::size_t ArrayEngine::Size() const {
  std::shared_lock lock(mutex_);
  return entries_.size();
}

}  // namespace kvstore
