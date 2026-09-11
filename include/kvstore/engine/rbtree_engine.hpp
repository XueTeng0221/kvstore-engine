#pragma once

#include <memory>
#include <shared_mutex>
#include <unordered_map>

#include "kvstore/engine/engine.hpp"

namespace kvstore {

class RBTreeEngine final : public IEngine {
 public:
  explicit RBTreeEngine(std::size_t capacity) : capacity_(capacity) {}
  Status Create(std::string_view key, std::string_view value, std::stop_token stop) override;
  Result<std::string> Get(std::string_view key, std::stop_token stop) const override;
  Status Delete(std::string_view key, std::stop_token stop) override;
  Status Modify(std::string_view key, std::string_view value, std::stop_token stop) override;
  Result<bool> Upsert(std::string_view key, std::string_view value, std::stop_token stop) override;
  Result<bool> Exists(std::string_view key, std::stop_token stop) const override;
  Result<std::int64_t> Increment(std::string_view key, std::int64_t delta,
                                 std::stop_token stop) override;
  Result<std::vector<Entry>> Scan(std::stop_token stop) const override;
  Result<std::vector<Entry>> Export(std::stop_token stop) const override;
  Status Import(const std::vector<Entry>& entries, std::stop_token stop) override;
  std::size_t Size() const override;
  std::size_t Capacity() const noexcept override { return capacity_; }
  std::string_view Name() const noexcept override { return "rbtree"; }
  [[nodiscard]] bool ValidateInvariants() const;

 private:
  enum class Color { kRed, kBlack };
  struct Node {
    std::string key;
    std::string value;
    Color color{Color::kRed};
    Node* parent{nullptr};
    Node* left{nullptr};
    Node* right{nullptr};
  };

  Node* Find(std::string_view key) const;
  Node* Minimum(Node* node) const;
  void RotateLeft(Node* node);
  void RotateRight(Node* node);
  void InsertFixup(Node* node);
  void Transplant(Node* from, Node* to);
  void DeleteFixup(Node* node, Node* parent);
  void EraseNode(Node* node);
  Node* InsertNode(std::string key, std::string value);
  void ScanNode(const Node* node, std::vector<Entry>& output) const;
  int ValidateNode(const Node* node, const std::string* minimum, const std::string* maximum,
                   bool& valid) const;

  const std::size_t capacity_;
  mutable std::shared_mutex mutex_;
  Node* root_{nullptr};
  std::unordered_map<Node*, std::unique_ptr<Node>> nodes_;
};

}  // namespace kvstore
