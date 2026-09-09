#include "kvstore/common/crc32.hpp"

#include <array>

namespace kvstore {
namespace {

constexpr auto MakeTable() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < table.size(); ++i) {
    std::uint32_t value = i;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value >> 1U) ^ ((value & 1U) != 0U ? 0xedb88320U : 0U);
    }
    table[i] = value;
  }
  return table;
}

constexpr auto kTable = MakeTable();

}  // namespace

std::uint32_t Crc32(ByteView data) noexcept {
  std::uint32_t crc = 0xffffffffU;
  for (const std::byte byte : data) {
    const auto index = static_cast<std::uint8_t>(crc ^ std::to_integer<std::uint8_t>(byte));
    crc = (crc >> 8U) ^ kTable[index];
  }
  return crc ^ 0xffffffffU;
}

}  // namespace kvstore
