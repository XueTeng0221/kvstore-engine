#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "kvstore/common/result.hpp"

namespace kvstore {

using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;

class ByteBuffer {
 public:
  explicit ByteBuffer(std::size_t limit) : limit_(limit) {}

  [[nodiscard]] Status Append(ByteView bytes) {
    if (bytes.size() > limit_ - data_.size()) {
      return {StatusCode::kLimitExceeded, "buffer limit exceeded"};
    }
    data_.insert(data_.end(), bytes.begin(), bytes.end());
    return Status::Ok();
  }

  [[nodiscard]] Result<Bytes> Read(std::size_t offset, std::size_t length) const {
    if (offset > data_.size() || length > data_.size() - offset) {
      return Status{StatusCode::kInvalidArgument, "buffer range out of bounds"};
    }
    const auto begin = data_.begin() + static_cast<std::ptrdiff_t>(offset);
    return Bytes(begin, begin + static_cast<std::ptrdiff_t>(length));
  }

  [[nodiscard]] ByteView view() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
  [[nodiscard]] std::size_t limit() const noexcept { return limit_; }
  void Clear() noexcept { data_.clear(); }

 private:
  std::size_t limit_;
  Bytes data_;
};

}  // namespace kvstore
