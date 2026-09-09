#pragma once

#include <optional>
#include <utility>

#include "kvstore/common/status.hpp"

namespace kvstore {

template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}
  Result(Status status)
      : status_(status.ok() ? Status{StatusCode::kInternal, "result constructed without a value"}
                            : std::move(status)) {}

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] const T& value() const& { return value_.value(); }
  [[nodiscard]] T& value() & { return value_.value(); }
  [[nodiscard]] T&& value() && { return std::move(value_.value()); }

 private:
  Status status_;
  std::optional<T> value_;
};

}  // namespace kvstore
