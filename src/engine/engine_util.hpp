#pragma once

#include <cstdint>
#include <stop_token>
#include <string_view>

#include "kvstore/common/result.hpp"

namespace kvstore::engine_internal {

[[nodiscard]] Status ValidateKey(std::string_view key);
[[nodiscard]] Status CheckCancelled(std::stop_token stop);
[[nodiscard]] Result<std::int64_t> ParseInteger(std::string_view value);
[[nodiscard]] Result<std::int64_t> AddChecked(std::int64_t value, std::int64_t delta);
[[nodiscard]] std::string FormatInteger(std::int64_t value);

}  // namespace kvstore::engine_internal
