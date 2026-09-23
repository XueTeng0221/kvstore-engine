#include "kvstore/engine/rbtree_engine.hpp"

#include <limits>
#include <mutex>

#include "engine_util.hpp"

namespace kvstore {

RBTreeEngine::Node* RBTreeEngine::Find(std::string_view key) const {
  Node* node = root_;
  while (node != nullptr) {
    if (key == node->key) return node;
    node = key < node->key ? node->left : node->right;
  }
  return nullptr;
}

RBTreeEngine::Node* RBTreeEngine::Minimum(Node* node) const {
  while (node != nullptr && node->left != nullptr) node = node->left;
  return node;
}

void RBTreeEngine::RotateLeft(Node* node) {
  Node* pivot = node->right;
  node->right = pivot->left;
  if (pivot->left != nullptr) pivot->left->parent = node;
  pivot->parent = node->parent;
  if (node->parent == nullptr) {
    root_ = pivot;
  } else if (node == node->parent->left) {
    node->parent->left = pivot;
  } else {
    node->parent->right = pivot;
  }
  pivot->left = node;
  node->parent = pivot;
}

void RBTreeEngine::RotateRight(Node* node) {
  Node* pivot = node->left;
  node->left = pivot->right;
  if (pivot->right != nullptr) pivot->right->parent = node;
  pivot->parent = node->parent;
  if (node->parent == nullptr) {
    root_ = pivot;
  } else if (node == node->parent->right) {
    node->parent->right = pivot;
  } else {
    node->parent->left = pivot;
  }
  pivot->right = node;
  node->parent = pivot;
}

void RBTreeEngine::InsertFixup(Node* node) {
  while (node->parent != nullptr && node->parent->color == Color::kRed) {
    Node* parent = node->parent;
    Node* grandparent = parent->parent;
    if (parent == grandparent->left) {
      Node* uncle = grandparent->right;
      if (uncle != nullptr && uncle->color == Color::kRed) {
        parent->color = Color::kBlack;
        uncle->color = Color::kBlack;
        grandparent->color = Color::kRed;
        node = grandparent;
      } else {
        if (node == parent->right) {
          node = parent;
          RotateLeft(node);
          parent = node->parent;
          grandparent = parent->parent;
        }
        parent->color = Color::kBlack;
        grandparent->color = Color::kRed;
        RotateRight(grandparent);
      }
    } else {
      Node* uncle = grandparent->left;
      if (uncle != nullptr && uncle->color == Color::kRed) {
        parent->color = Color::kBlack;
        uncle->color = Color::kBlack;
        grandparent->color = Color::kRed;
        node = grandparent;
      } else {
        if (node == parent->left) {
          node = parent;
          RotateRight(node);
          parent = node->parent;
          grandparent = parent->parent;
        }
        parent->color = Color::kBlack;
        grandparent->color = Color::kRed;
        RotateLeft(grandparent);
      }
    }
  }
  root_->color = Color::kBlack;
}

RBTreeEngine::Node* RBTreeEngine::InsertNode(std::string key, std::string value) {
  auto owned = std::make_unique<Node>();
  owned->key = std::move(key);
  owned->value = std::move(value);
  Node* node = owned.get();
  Node* parent = nullptr;
  Node* cursor = root_;
  while (cursor != nullptr) {
    parent = cursor;
    cursor = node->key < cursor->key ? cursor->left : cursor->right;
  }
  node->parent = parent;
  if (parent == nullptr) {
    root_ = node;
  } else if (node->key < parent->key) {
    parent->left = node;
  } else {
    parent->right = node;
  }
  nodes_.emplace(node, std::move(owned));
  InsertFixup(node);
  return node;
}

void RBTreeEngine::Transplant(Node* from, Node* to) {
  if (from->parent == nullptr) {
    root_ = to;
  } else if (from == from->parent->left) {
    from->parent->left = to;
  } else {
    from->parent->right = to;
  }
  if (to != nullptr) to->parent = from->parent;
}

void RBTreeEngine::DeleteFixup(Node* node, Node* parent) {
  const auto color = [](const Node* value) {
    return value == nullptr ? Color::kBlack : value->color;
  };
  while (node != root_ && color(node) == Color::kBlack) {
    if (parent == nullptr) break;
    if (node == parent->left) {
      Node* sibling = parent->right;
      if (color(sibling) == Color::kRed) {
        sibling->color = Color::kBlack;
        parent->color = Color::kRed;
        RotateLeft(parent);
        sibling = parent->right;
      }
      if (sibling == nullptr) {
        node = parent;
        parent = node->parent;
        continue;
      }
      if (color(sibling->left) == Color::kBlack && color(sibling->right) == Color::kBlack) {
        sibling->color = Color::kRed;
        node = parent;
        parent = node->parent;
      } else {
        if (color(sibling->right) == Color::kBlack) {
          if (sibling->left != nullptr) sibling->left->color = Color::kBlack;
          sibling->color = Color::kRed;
          RotateRight(sibling);
          sibling = parent->right;
        }
        sibling->color = parent->color;
        parent->color = Color::kBlack;
        if (sibling->right != nullptr) sibling->right->color = Color::kBlack;
        RotateLeft(parent);
        node = root_;
        parent = nullptr;
      }
    } else {
      Node* sibling = parent->left;
      if (color(sibling) == Color::kRed) {
        sibling->color = Color::kBlack;
        parent->color = Color::kRed;
        RotateRight(parent);
        sibling = parent->left;
      }
      if (sibling == nullptr) {
        node = parent;
        parent = node->parent;
        continue;
      }
      if (color(sibling->right) == Color::kBlack && color(sibling->left) == Color::kBlack) {
        sibling->color = Color::kRed;
        node = parent;
        parent = node->parent;
      } else {
        if (color(sibling->left) == Color::kBlack) {
          if (sibling->right != nullptr) sibling->right->color = Color::kBlack;
          sibling->color = Color::kRed;
          RotateLeft(sibling);
          sibling = parent->left;
        }
        sibling->color = parent->color;
        parent->color = Color::kBlack;
        if (sibling->left != nullptr) sibling->left->color = Color::kBlack;
        RotateRight(parent);
        node = root_;
        parent = nullptr;
      }
    }
  }
  if (node != nullptr) node->color = Color::kBlack;
}

void RBTreeEngine::EraseNode(Node* node) {
  Node* moved = node;
  Color original_color = moved->color;
  Node* replacement = nullptr;
  Node* replacement_parent = nullptr;
  if (node->left == nullptr) {
    replacement = node->right;
    replacement_parent = node->parent;
    Transplant(node, node->right);
  } else if (node->right == nullptr) {
    replacement = node->left;
    replacement_parent = node->parent;
    Transplant(node, node->left);
  } else {
    moved = Minimum(node->right);
    original_color = moved->color;
    replacement = moved->right;
    if (moved->parent == node) {
      replacement_parent = moved;
      if (replacement != nullptr) replacement->parent = moved;
    } else {
      replacement_parent = moved->parent;
      Transplant(moved, moved->right);
      moved->right = node->right;
      moved->right->parent = moved;
    }
    Transplant(node, moved);
    moved->left = node->left;
    moved->left->parent = moved;
    moved->color = node->color;
  }
  if (original_color == Color::kBlack) DeleteFixup(replacement, replacement_parent);
  nodes_.erase(node);
}

Status RBTreeEngine::Create(std::string_view key, std::string_view value, std::stop_token stop) {
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

Result<std::string> RBTreeEngine::Get(std::string_view key, std::stop_token stop) const {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::shared_lock lock(mutex_);
  const Node* node = Find(key);
  if (node == nullptr) return Status{StatusCode::kNotFound, "key not found"};
  return node->value;
}

Status RBTreeEngine::Delete(std::string_view key, std::stop_token stop) {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::unique_lock lock(mutex_);
  Node* node = Find(key);
  if (node == nullptr) return {StatusCode::kNotFound, "key not found"};
  if (stop.stop_requested()) return {StatusCode::kCancelled, "operation cancelled"};
  EraseNode(node);
  return Status::Ok();
}

Status RBTreeEngine::Modify(std::string_view key, std::string_view value, std::stop_token stop) {
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

Result<bool> RBTreeEngine::Upsert(std::string_view key, std::string_view value,
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

Result<bool> RBTreeEngine::Exists(std::string_view key, std::stop_token stop) const {
  auto status = engine_internal::ValidateKey(key);
  if (!status.ok()) return status;
  status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::shared_lock lock(mutex_);
  return Find(key) != nullptr;
}

Result<std::int64_t> RBTreeEngine::Increment(std::string_view key, std::int64_t delta,
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

void RBTreeEngine::ScanNode(const Node* node, std::vector<Entry>& output) const {
  if (node == nullptr) return;
  ScanNode(node->left, output);
  output.push_back({node->key, node->value});
  ScanNode(node->right, output);
}

Result<std::vector<Entry>> RBTreeEngine::Scan(std::stop_token stop) const {
  auto status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  std::shared_lock lock(mutex_);
  std::vector<Entry> output;
  output.reserve(nodes_.size());
  ScanNode(root_, output);
  return output;
}

Result<std::vector<Entry>> RBTreeEngine::Export(std::stop_token stop) const { return Scan(stop); }

Status RBTreeEngine::Import(const std::vector<Entry>& entries, std::stop_token stop) {
  auto status = engine_internal::CheckCancelled(stop);
  if (!status.ok()) return status;
  if (entries.size() > capacity_) return {StatusCode::kLimitExceeded, "import exceeds capacity"};
  RBTreeEngine replacement(capacity_);
  for (const auto& entry : entries) {
    status = replacement.Create(entry.key, entry.value, stop);
    if (!status.ok()) return status;
  }
  std::unique_lock lock(mutex_);
  if (stop.stop_requested()) return {StatusCode::kCancelled, "operation cancelled"};
  std::swap(root_, replacement.root_);
  nodes_.swap(replacement.nodes_);
  return Status::Ok();
}

std::size_t RBTreeEngine::Size() const {
  std::shared_lock lock(mutex_);
  return nodes_.size();
}

Result<std::size_t> RBTreeEngine::DataBytes() const {
  std::shared_lock lock(mutex_);
  std::size_t bytes = 0;
  for (const auto& [node, owner] : nodes_) {
    static_cast<void>(node);
    if (owner->key.size() > std::numeric_limits<std::size_t>::max() - bytes ||
        owner->value.size() > std::numeric_limits<std::size_t>::max() - bytes - owner->key.size())
      return Status{StatusCode::kLimitExceeded, "engine data size overflow"};
    bytes += owner->key.size() + owner->value.size();
  }
  return bytes;
}

int RBTreeEngine::ValidateNode(const Node* node, const std::string* minimum,
                               const std::string* maximum, bool& valid) const {
  if (node == nullptr) return 1;
  if ((minimum != nullptr && node->key <= *minimum) ||
      (maximum != nullptr && node->key >= *maximum) ||
      (node->left != nullptr && node->left->parent != node) ||
      (node->right != nullptr && node->right->parent != node)) {
    valid = false;
  }
  if (node->color == Color::kRed &&
      ((node->left != nullptr && node->left->color == Color::kRed) ||
       (node->right != nullptr && node->right->color == Color::kRed))) {
    valid = false;
  }
  const int left_height = ValidateNode(node->left, minimum, &node->key, valid);
  const int right_height = ValidateNode(node->right, &node->key, maximum, valid);
  if (left_height != right_height) valid = false;
  return left_height + (node->color == Color::kBlack ? 1 : 0);
}

bool RBTreeEngine::ValidateInvariants() const {
  std::shared_lock lock(mutex_);
  if (root_ == nullptr) return nodes_.empty();
  bool valid = root_->parent == nullptr && root_->color == Color::kBlack;
  static_cast<void>(ValidateNode(root_, nullptr, nullptr, valid));
  return valid;
}

}  // namespace kvstore
