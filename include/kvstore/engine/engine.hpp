#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "kvstore/common/result.hpp"

namespace kvstore {

struct Entry {
  std::string key;
  std::string value;

  friend bool operator==(const Entry&, const Entry&) = default;
};

class IEngine {
 public:
  virtual ~IEngine() = default;

  [[nodiscard]] virtual Status Create(std::string_view key, std::string_view value,
                                      std::stop_token stop = {}) = 0;
  [[nodiscard]] virtual Result<std::string> Get(std::string_view key,
                                                std::stop_token stop = {}) const = 0;
  [[nodiscard]] virtual Status Delete(std::string_view key, std::stop_token stop = {}) = 0;
  [[nodiscard]] virtual Status Modify(std::string_view key, std::string_view value,
                                      std::stop_token stop = {}) = 0;
  // Returns true when a new key was created and false when an existing value was replaced.
  [[nodiscard]] virtual Result<bool> Upsert(std::string_view key, std::string_view value,
                                            std::stop_token stop = {}) = 0;
  [[nodiscard]] virtual Result<bool> Exists(std::string_view key,
                                            std::stop_token stop = {}) const = 0;
  [[nodiscard]] virtual Result<std::int64_t> Increment(std::string_view key, std::int64_t delta,
                                                       std::stop_token stop = {}) = 0;
  [[nodiscard]] virtual Result<std::vector<Entry>> Scan(std::stop_token stop = {}) const = 0;
  [[nodiscard]] virtual Result<std::vector<Entry>> Export(std::stop_token stop = {}) const = 0;
  [[nodiscard]] virtual Status Import(const std::vector<Entry>& entries,
                                      std::stop_token stop = {}) = 0;
  [[nodiscard]] virtual std::size_t Size() const = 0;
  [[nodiscard]] virtual std::string_view Name() const noexcept = 0;
};

}  // namespace kvstore
