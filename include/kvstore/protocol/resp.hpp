#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kvstore/command/command.hpp"
#include "kvstore/common/result.hpp"

namespace kvstore {

class RespParser {
 public:
  explicit RespParser(std::size_t max_frame_bytes) : max_frame_bytes_(max_frame_bytes) {}
  [[nodiscard]] Status Feed(std::string_view bytes);
  [[nodiscard]] Result<std::vector<Command>> ParseAvailable();
  [[nodiscard]] bool has_buffered_data() const noexcept { return !buffer_.empty(); }
  [[nodiscard]] std::size_t buffered_bytes() const noexcept { return buffer_.size(); }

 private:
  std::string buffer_;
  std::size_t max_frame_bytes_;
};

[[nodiscard]] Result<Command> CommandFromResp(const std::vector<std::string>& parts);
[[nodiscard]] std::string EncodeResp(const CommandResponse& response);

}  // namespace kvstore
