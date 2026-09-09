#pragma once

#include <memory>
#include <random>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "kvstore/engine/engine.hpp"

namespace kvstore {

class SkipListEngine final : public IEngine {
 public:
  SkipListEngine(std::size_t capacity, std::size_t max_level = 20, double probability = 0.5,
                 std::uint64_t seed = 0xc0ffee);
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
  std::string_view Name() const noexcept override { return "skiplist"; }
  [[nodiscard]] bool ValidateInvariants() const;

 private:
  struct Node {
    Node(std::string key_value, std::string data, std::size_t levels)
        : key(std::move(key_value)), value(std::move(data)), forward(levels, nullptr) {}
    std::string key;
    std::string value;
    std::vector<Node*> forward;
  };

  Node* Find(std::string_view key) const;
  Node* InsertNode(std::string key, std::string value);
  bool EraseNode(std::string_view key);
  std::size_t RandomLevel();

  const std::size_t capacity_;
  const std::size_t max_level_;
  const double probability_;
  const std::uint64_t seed_;
  mutable std::shared_mutex mutex_;
  std::mt19937_64 generator_;
  std::unique_ptr<Node> head_;
  std::unordered_map<Node*, std::unique_ptr<Node>> nodes_;
  std::size_t current_level_{1};
};

}  // namespace kvstore
