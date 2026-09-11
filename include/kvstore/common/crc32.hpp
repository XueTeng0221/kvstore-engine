#pragma once

#include <cstdint>
#include <span>

#include "kvstore/common/byte_buffer.hpp"

namespace kvstore {

[[nodiscard]] std::uint32_t Crc32(ByteView data) noexcept;
[[nodiscard]] std::uint32_t Crc32Parts(std::span<const ByteView> parts) noexcept;

}  // namespace kvstore
