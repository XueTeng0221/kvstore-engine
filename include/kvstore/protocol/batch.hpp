#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "kvstore/command/command.hpp"
#include "kvstore/common/result.hpp"

namespace kvstore {

class BatchParser {
 public:
  explicit BatchParser(std::size_t max_records, std::size_t max_frame)
      : max_records_(max_records), max_frame_(max_frame) {}
  [[nodiscard]] Status Feed(std::string_view bytes);
  [[nodiscard]] Result<std::vector<Command>> ParseAvailable();

 private:
  std::string buffer_;
  std::size_t max_records_;
  std::size_t max_frame_;
};

[[nodiscard]] std::string EncodeBatch(const std::vector<CommandResponse>& responses);

}  // namespace kvstore
