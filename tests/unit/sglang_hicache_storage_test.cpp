#include <gtest/gtest.h>
#include "kvstore/integration/sglang/hicache_storage.hpp"

namespace {
using namespace kvstore::integration::sglang;
TEST(SglangHiCache, CanonicalRadixMetadata) {
  const std::uint32_t tokens[] = {4, 5, 6};
  PrefixQuery q{{"qwen-rev", "tok-rev", 0, 24, 14, 128, 16,
                kvstore::kvcache::DType::kBFloat16,
                {kvstore::kvcache::DeviceKind::kCuda, 0}},
               tokens,
               {},
               false};
  auto result = CanonicalManifest(q, "tenant", "Qwen2.5-0.5B");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().cache_format, "sglang-hicache-interface_v1");
  EXPECT_EQ(result.value().layout, kvstore::kvcache::TensorLayout::kBlockMajor);
  EXPECT_EQ(result.value().shape[2], 3U);
  EXPECT_EQ(result.value().token_count, 3U);
}
TEST(SglangHiCache, RejectsEmptyRadixPrefix) {
  PrefixQuery q{};
  EXPECT_FALSE(CanonicalManifest(q, "tenant", "Qwen2.5-0.5B").ok());
}
}
