#pragma once

#include <cstdint>
#include <mutex>
#include <random>

namespace kvstore {

class IRandom {
 public:
  virtual ~IRandom() = default;
  [[nodiscard]] virtual std::uint64_t Next() = 0;
};

class SeededRandom final : public IRandom {
 public:
  explicit SeededRandom(std::uint64_t seed) : generator_(seed) {}
  [[nodiscard]] std::uint64_t Next() override {
    std::scoped_lock lock(mutex_);
    return generator_();
  }

 private:
  std::mutex mutex_;
  std::mt19937_64 generator_;
};

}  // namespace kvstore
