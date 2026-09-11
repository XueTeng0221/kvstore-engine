#pragma once

#include <cstddef>
#include <limits>
#include <string_view>
#include <vector>

#include "kvstore/command/command.hpp"
#include "kvstore/common/result.hpp"

namespace kvstore {

class BatchParser {
 public:
  explicit BatchParser(std::size_t max_records, std::size_t max_frame, std::size_t max_buffer = 0,
                       std::size_t max_key = std::numeric_limits<std::size_t>::max(),
                       std::size_t max_value = std::numeric_limits<std::size_t>::max())
      : max_records_(max_records),
        max_frame_(max_frame),
        max_buffer_(max_buffer == 0 ? max_frame : max_buffer),
        max_key_(max_key),
        max_value_(max_value) {}
  [[nodiscard]] Status Feed(std::string_view bytes);
  [[nodiscard]] Result<std::vector<Command>> ParseAvailable();
  [[nodiscard]] Result<std::vector<std::vector<Command>>> ParseAvailableFrames();
  [[nodiscard]] bool has_buffered_data() const noexcept { return !buffer_.empty(); }

 private:
  std::string buffer_;
  std::size_t max_records_;
  std::size_t max_frame_;
  std::size_t max_buffer_;
  std::size_t max_key_;
  std::size_t max_value_;
};

[[nodiscard]] std::string EncodeBatch(const std::vector<CommandResponse>& responses);

}  // namespace kvstore
