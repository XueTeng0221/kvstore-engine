#pragma once

#include <chrono>

namespace kvstore {

class IClock {
 public:
  using TimePoint = std::chrono::steady_clock::time_point;
  virtual ~IClock() = default;
  [[nodiscard]] virtual TimePoint Now() const noexcept = 0;
};

class SteadyClock final : public IClock {
 public:
  [[nodiscard]] TimePoint Now() const noexcept override { return std::chrono::steady_clock::now(); }
};

class ManualClock final : public IClock {
 public:
  explicit ManualClock(TimePoint now = {}) : now_(now) {}
  [[nodiscard]] TimePoint Now() const noexcept override { return now_; }
  void Advance(std::chrono::steady_clock::duration duration) noexcept { now_ += duration; }

 private:
  TimePoint now_;
};

}  // namespace kvstore
