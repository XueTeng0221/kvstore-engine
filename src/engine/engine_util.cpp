#include "engine_util.hpp"

#include <charconv>
#include <limits>
#include <string>

namespace kvstore::engine_internal {

Status ValidateKey(std::string_view key) {
  if (key.empty()) return {StatusCode::kInvalidArgument, "key must not be empty"};
  return Status::Ok();
}

Status CheckCancelled(std::stop_token stop) {
  if (stop.stop_requested()) return {StatusCode::kCancelled, "operation cancelled"};
  return Status::Ok();
}

Result<std::int64_t> ParseInteger(std::string_view value) {
  if (value.empty() || value.front() == '+' || value == "-0" ||
      (value.size() > 1 && value.front() == '0')) {
    return Status{StatusCode::kInvalidArgument, "value is not a canonical integer"};
  }
  std::int64_t parsed = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    return Status{StatusCode::kInvalidArgument, "value is not a signed 64-bit integer"};
  }
  return parsed;
}

Result<std::int64_t> AddChecked(std::int64_t value, std::int64_t delta) {
  if ((delta > 0 && value > std::numeric_limits<std::int64_t>::max() - delta) ||
      (delta < 0 && value < std::numeric_limits<std::int64_t>::min() - delta)) {
    return Status{StatusCode::kInvalidArgument, "integer overflow"};
  }
  return value + delta;
}

std::string FormatInteger(std::int64_t value) { return std::to_string(value); }

}  // namespace kvstore::engine_internal
