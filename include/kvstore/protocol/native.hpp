#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "kvstore/command/command.hpp"
#include "kvstore/common/result.hpp"

namespace kvstore {

class NativeParser {
 public:
  NativeParser(std::size_t max_key_bytes, std::size_t max_value_bytes, std::size_t max_frame_bytes)
      : max_key_bytes_(max_key_bytes),
        max_value_bytes_(max_value_bytes),
        max_frame_bytes_(max_frame_bytes) {}
  [[nodiscard]] Status Feed(std::string_view bytes);
  [[nodiscard]] Result<std::vector<Command>> ParseAvailable();

 private:
  std::string buffer_;
  std::size_t max_key_bytes_;
  std::size_t max_value_bytes_;
  std::size_t max_frame_bytes_;
};

[[nodiscard]] std::string EncodeNative(const CommandResponse& response);

}  // namespace kvstore
