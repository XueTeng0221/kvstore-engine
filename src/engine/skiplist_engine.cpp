#include "kvstore/engine/skiplist_engine.hpp"

#include <mutex>
#include <unordered_set>

#include "engine_util.hpp"

namespace kvstore {

SkipListEngine::SkipListEngine(std::size_t capacity, std::size_t max_level, double probability,
                               std::uint64_t seed)
    : capacity_(capacity),
      max_level_(max_level),
      probability_(probability),
      seed_(seed),
      generator_(seed),
      head_(std::make_unique<Node>("", "", max_level)) {}

std::size_t SkipListEngine::RandomLevel() {
  std::size_t level = 1;
  std::bernoulli_distribution promote(probability_);
  while (level < max_level_ && promote(generator_)) ++level;
  return level;
}

SkipListEngine::Node* SkipListEngine::Find(std::string_view key) const {
  Node* cursor = head_.get();
  for (std::size_t level = current_level_; level > 0; --level) {
    const std::size_t index = level - 1;
    while (cursor->forward[index] != nullptr && cursor->forward[index]->key < key) {
      cursor = cursor->forward[index];
    }
  }
  cursor = cursor->forward[0];
  return cursor != nullptr && cursor->key == key ? cursor : nullptr;
}

SkipListEngine::Node* SkipListEngine::InsertNode(std::string key, std::string value) {
  std::vector<Node*> update(max_level_, nullptr);
  Node* cursor = head_.get();
  for (std::size_t level = current_level_; level > 0; --level) {
    const std::size_t index = level - 1;
    while (cursor->forward[index] != nullptr && cursor->forward[index]->key < key) {
      cursor = cursor->forward[index];
    }
    update[index] = cursor;
  }
  const std::size_t node_level = RandomLevel();
  if (node_level > current_level_) {
    for (std::size_t index = current_level_; index < node_level; ++index)
      update[index] = head_.get();
    current_level_ = node_level;
  }
  auto owned = std::make_unique<Node>(std::move(key), std::move(value), node_level);
  Node* node = owned.get();
  for (std::size_t index = 0; index < node_level; ++index) {
    node->forward[index] = update[index]->forward[index];
    update[index]->forward[index] = node;
  }
  nodes_.emplace(node, std::move(owned));
  return node;
}

bool SkipListEngine::EraseNode(std::string_view key) {
  std::vector<Node*> update(max_level_, nullptr);
  Node* cursor = head_.get();
  for (std::size_t level = current_level_; level > 0; --level) {
    const std::size_t index = level - 1;
    while (cursor->forward[index] != nullptr && cursor->forward[index]->key < key) {
      cursor = cursor->forward[index];
    }
    update[index] = cursor;
  }
  Node* node = cursor->forward[0];
  if (node == nullptr || node->key != key) return false;
  for (std::size_t index = 0; index < node->forward.size(); ++index) {
    if (update[index]->forward[index] == node) update[index]->forward[index] = node->forward[index];
  }
  nodes_.erase(node);
  while (current_level_ > 1 && head_->forward[current_level_ - 1] == nullptr) --current_level_;
  return true;
}

Status SkipListEngine::Create(std::string_view key, std::string_view value, std::stop_token stop) {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::unique_lock lock(mutex_);
  if (Find(key) != nullptr) return {StatusCode::kAlreadyExists, "key already exists"};
  if (nodes_.size() >= capacity_) return {StatusCode::kLimitExceeded, "engine capacity reached"};
  if (stop.stop_requested()) return {StatusCode::kCancelled, "operation cancelled"};
  static_cast<void>(InsertNode(std::string(key), std::string(value)));
  return Status::Ok();
}

Result<std::string> SkipListEngine::Get(std::string_view key, std::stop_token stop) const {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::shared_lock lock(mutex_);
  const Node* node = Find(key);
  if (node == nullptr) return Status{StatusCode::kNotFound, "key not found"};
  return node->value;
}

Status SkipListEngine::Delete(std::string_view key, std::stop_token stop) {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::unique_lock lock(mutex_);
  if (Find(key) == nullptr) return {StatusCode::kNotFound, "key not found"};
  if (stop.stop_requested()) return {StatusCode::kCancelled, "operation cancelled"};
  static_cast<void>(EraseNode(key));
  return Status::Ok();
}

Status SkipListEngine::Modify(std::string_view key, std::string_view value, std::stop_token stop) {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::unique_lock lock(mutex_);
  Node* node = Find(key);
  if (node == nullptr) return {StatusCode::kNotFound, "key not found"};
  if (stop.stop_requested()) return {StatusCode::kCancelled, "operation cancelled"};
  node->value.assign(value);
  return Status::Ok();
}

Result<bool> SkipListEngine::Upsert(std::string_view key, std::string_view value,
                                    std::stop_token stop) {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::unique_lock lock(mutex_);
  Node* node = Find(key);
  if (node != nullptr) {
    if (stop.stop_requested()) return Status{StatusCode::kCancelled, "operation cancelled"};
    node->value.assign(value);
    return false;
  }
  if (nodes_.size() >= capacity_)
    return Status{StatusCode::kLimitExceeded, "engine capacity reached"};
  if (stop.stop_requested()) return Status{StatusCode::kCancelled, "operation cancelled"};
  static_cast<void>(InsertNode(std::string(key), std::string(value)));
  return true;
}

Result<bool> SkipListEngine::Exists(std::string_view key, std::stop_token stop) const {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::shared_lock lock(mutex_);
  return Find(key) != nullptr;
}

Result<std::int64_t> SkipListEngine::Increment(std::string_view key, std::int64_t delta,
                                               std::stop_token stop) {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::unique_lock lock(mutex_);
  Node* node = Find(key);
  std::int64_t current = 0;
  if (node != nullptr) {
    auto parsed = engine_internal::ParseInteger(node->value);
    if (!parsed.ok()) return parsed.status();
    current = parsed.value();
  } else if (nodes_.size() >= capacity_) {
    return Status{StatusCode::kLimitExceeded, "engine capacity reached"};
  }
  auto updated = engine_internal::AddChecked(current, delta);
  if (!updated.ok()) return updated.status();
  if (stop.stop_requested()) return Status{StatusCode::kCancelled, "operation cancelled"};
  if (node == nullptr) {
    static_cast<void>(
        InsertNode(std::string(key), engine_internal::FormatInteger(updated.value())));
  } else {
    node->value = engine_internal::FormatInteger(updated.value());
  }
  return updated.value();
}

Result<std::vector<Entry>> SkipListEngine::Scan(std::stop_token stop) const {
  auto status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::shared_lock lock(mutex_);
  std::vector<Entry> output;
  output.reserve(nodes_.size());
  Node* node = head_->forward[0];
  while (node != nullptr) {
    output.push_back({node->key, node->value});
    node = node->forward[0];
  }
  return output;
}

Result<std::vector<Entry>> SkipListEngine::Export(std::stop_token stop) const { return Scan(stop); }

Status SkipListEngine::Import(const std::vector<Entry>& entries, std::stop_token stop) {
  auto status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  if (entries.size() > capacity_) return {StatusCode::kLimitExceeded, "import exceeds capacity"};
  SkipListEngine replacement(capacity_, max_level_, probability_, seed_);
  for (const auto& entry : entries) {
    status = replacement.Create(entry.key, entry.value, stop);
    if (!status.ok()) return status;
  }
  std::unique_lock lock(mutex_);
  if (stop.stop_requested()) return {StatusCode::kCancelled, "operation cancelled"};
  std::swap(generator_, replacement.generator_);
  head_.swap(replacement.head_);
  nodes_.swap(replacement.nodes_);
  std::swap(current_level_, replacement.current_level_);
  return Status::Ok();
}

std::size_t SkipListEngine::Size() const {
  std::shared_lock lock(mutex_);
  return nodes_.size();
}

bool SkipListEngine::ValidateInvariants() const {
  std::shared_lock lock(mutex_);
  if (current_level_ == 0 || current_level_ > max_level_) return false;
  std::unordered_set<const Node*> base_nodes;
  const Node* previous = nullptr;
  for (Node* node = head_->forward[0]; node != nullptr; node = node->forward[0]) {
    if ((previous != nullptr && previous->key >= node->key) || node->forward.empty()) return false;
    base_nodes.insert(node);
    previous = node;
  }
  if (base_nodes.size() != nodes_.size()) return false;
  for (std::size_t level = 1; level < current_level_; ++level) {
    previous = nullptr;
    for (Node* node = head_->forward[level]; node != nullptr; node = node->forward[level]) {
      if (node->forward.size() <= level || !base_nodes.contains(node) ||
          (previous != nullptr && previous->key >= node->key)) {
        return false;
      }
      previous = node;
    }
  }
  for (std::size_t level = current_level_; level < max_level_; ++level) {
    if (head_->forward[level] != nullptr) return false;
  }
  return true;
}

}  // namespace kvstore
