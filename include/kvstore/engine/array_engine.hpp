#pragma once

#include <shared_mutex>
#include <vector>

#include "kvstore/engine/engine.hpp"

namespace kvstore {

class ArrayEngine final : public IEngine {
 public:
  explicit ArrayEngine(std::size_t capacity, std::size_t reserve = 0);
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
  std::string_view Name() const noexcept override { return "array"; }

 private:
  using Iterator = std::vector<Entry>::iterator;
  using ConstIterator = std::vector<Entry>::const_iterator;
  Iterator Find(std::string_view key);
  ConstIterator Find(std::string_view key) const;

  const std::size_t capacity_;
  mutable std::shared_mutex mutex_;
  std::vector<Entry> entries_;
};

}  // namespace kvstore
