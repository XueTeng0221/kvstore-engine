#pragma once

#include <cstdint>

#include "kvstore/common/byte_buffer.hpp"

namespace kvstore {

[[nodiscard]] std::uint32_t Crc32(ByteView data) noexcept;

}  // namespace kvstore
