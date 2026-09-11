#include <cstddef>
#include <cstdint>
#include <string_view>

#include "kvstore/protocol/batch.hpp"
#include "kvstore/protocol/native.hpp"
#include "kvstore/protocol/resp.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  kvstore::RespParser resp(1U << 20U);
  if (resp.Feed(input).ok()) static_cast<void>(resp.ParseAvailable());
  kvstore::NativeParser native(4096, 1U << 18U, 1U << 20U);
  if (native.Feed(input).ok()) static_cast<void>(native.ParseAvailable());
  kvstore::BatchParser batch(128, 1U << 20U, 1U << 20U);
  if (batch.Feed(input).ok()) static_cast<void>(batch.ParseAvailableFrames());
  return 0;
}
