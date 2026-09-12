#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace kvstore {

enum class StatusCode : std::uint16_t {
  kOk = 0,
  kNotFound = 1,
  kAlreadyExists = 2,
  kInvalidArgument = 3,
  kLimitExceeded = 4,
  kIoError = 5,
  kCorruption = 6,
  kUnsupported = 7,
  kCancelled = 8,
  kReadOnly = 9,
  kBusy = 10,
  kInternal = 11,
  kDeadlineExceeded = 12,
};

class Status {
 public:
  Status() = default;
  Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

  [[nodiscard]] static Status Ok() { return {}; }
  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::kOk; }
  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] std::uint16_t wire_code() const noexcept {
    return static_cast<std::uint16_t>(code_);
  }
  [[nodiscard]] std::string_view message() const noexcept { return message_; }
  friend bool operator==(const Status&, const Status&) = default;
  [[nodiscard]] std::string_view name() const noexcept {
    switch (code_) {
      case StatusCode::kOk:
        return "OK";
      case StatusCode::kNotFound:
        return "NOT_FOUND";
      case StatusCode::kAlreadyExists:
        return "ALREADY_EXISTS";
      case StatusCode::kInvalidArgument:
        return "INVALID_ARGUMENT";
      case StatusCode::kLimitExceeded:
        return "LIMIT_EXCEEDED";
      case StatusCode::kIoError:
        return "IO_ERROR";
      case StatusCode::kCorruption:
        return "CORRUPTION";
      case StatusCode::kUnsupported:
        return "UNSUPPORTED";
      case StatusCode::kCancelled:
        return "CANCELLED";
      case StatusCode::kReadOnly:
        return "READ_ONLY";
      case StatusCode::kBusy:
        return "BUSY";
      case StatusCode::kInternal:
        return "INTERNAL";
      case StatusCode::kDeadlineExceeded:
        return "DEADLINE_EXCEEDED";
    }
    return "INTERNAL";
  }

 private:
  StatusCode code_{StatusCode::kOk};
  std::string message_;
};

}  // namespace kvstore
