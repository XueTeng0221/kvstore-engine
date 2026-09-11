#pragma once

#include <shared_mutex>
#include <unordered_map>

#include "kvstore/engine/engine.hpp"

namespace kvstore {

class HashEngine final : public IEngine {
 public:
  HashEngine(std::size_t capacity, std::size_t initial_buckets = 16, float max_load_factor = 0.75F);
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
  std::string_view Name() const noexcept override { return "hash"; }

 private:
  const std::size_t capacity_;
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, std::string> entries_;
};

}  // namespace kvstore
