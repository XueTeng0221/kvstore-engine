#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "kvstore/kvcache/match_index.hpp"

namespace kvstore::kvcache {
namespace {

TensorManifest Manifest(std::uint64_t tokens, std::byte payload_byte) {
  TensorManifest manifest;
  manifest.tenant_id = "tenant-a";
  manifest.model_id = "model";
  manifest.model_revision = "model-v1";
  manifest.tokenizer_revision = "tokenizer-v1";
  manifest.cache_format = "connector-v1";
  manifest.cache_format_version = 1;
  std::vector<std::uint32_t> token_ids;
  for (std::uint32_t token = 0; token < tokens; ++token) {
    token_ids.push_back(token + 1U);
  }
  manifest.token_digest = TokenDigest(token_ids).value();
  manifest.token_count = tokens;
  manifest.layer_count = 1;
  manifest.dtype = DType::kFloat16;
  manifest.shape = {2, 1, tokens, 2, 4};
  manifest.axis_order = {TensorAxis::kKeyValue, TensorAxis::kLayer, TensorAxis::kToken,
                         TensorAxis::kHead, TensorAxis::kHeadDimension};
  manifest.strides_bytes = {tokens * 16U, tokens * 16U, 16, 8, 2};
  manifest.layout = TensorLayout::kLayerMajor;
  manifest.device = {DeviceKind::kCuda, 0};
  manifest.payload_bytes = tokens * 32U;
  manifest.chunk_bytes = 64;
  manifest.chunk_count = static_cast<std::uint32_t>((manifest.payload_bytes + 63U) / 64U);
  const Bytes payload(static_cast<std::size_t>(manifest.payload_bytes), payload_byte);
  manifest.payload_digest = Sha256(payload).value();
  return manifest;
}

std::vector<std::uint32_t> Tokens(std::uint64_t count) {
  std::vector<std::uint32_t> result;
  for (std::uint32_t token = 0; token < count; ++token) {
    result.push_back(token + 1U);
  }
  return result;
}

TEST(KvCacheMatchTest, ExactReturnsAllHitLayersAndChunks) {
  MatchIndex index;
  const TensorManifest manifest = Manifest(8, std::byte{'x'});
  const auto key = CanonicalCacheKey(manifest);
  ASSERT_TRUE(key.ok());
  ASSERT_TRUE(index.Insert(manifest, key.value()).ok());

  TensorManifest query = manifest;
  query.payload_digest = {};
  const auto tokens = Tokens(query.token_count);
  const auto match = index.Exact(query, tokens);
  ASSERT_TRUE(match.ok());
  EXPECT_EQ(match.value().key, key.value());
  EXPECT_EQ(match.value().hit_tokens, 8U);
  EXPECT_EQ(match.value().layer_count, 1U);
  EXPECT_EQ(match.value().chunk_indices, (std::vector<std::uint32_t>{0, 1, 2, 3}));
  EXPECT_TRUE(match.value().missing_tokens.empty());
}

TEST(KvCacheMatchTest, LongestPrefixReturnsMissingTokenRangeInInputOrder) {
  MatchIndex index;
  const TensorManifest short_entry = Manifest(4, std::byte{'x'});
  const TensorManifest long_entry = Manifest(8, std::byte{'x'});
  ASSERT_TRUE(index.Insert(short_entry, CanonicalCacheKey(short_entry).value()).ok());
  ASSERT_TRUE(index.Insert(long_entry, CanonicalCacheKey(long_entry).value()).ok());

  TensorManifest query = Manifest(12, std::byte{'x'});
  query.payload_digest = {};
  const std::array<std::uint64_t, 3> prefixes{4, 8, 12};
  const auto tokens = Tokens(query.token_count);
  const auto match = index.LongestPrefix(query, tokens, prefixes);
  ASSERT_TRUE(match.ok());
  EXPECT_EQ(match.value().hit_tokens, 8U);
  EXPECT_EQ(match.value().missing_tokens, (std::vector<TokenRange>{{8, 12}}));
}

TEST(KvCacheMatchTest, RejectsCrossTenantLayoutAndDigestMismatch) {
  MatchIndex index;
  const TensorManifest entry = Manifest(4, std::byte{'x'});
  ASSERT_TRUE(index.Insert(entry, CanonicalCacheKey(entry).value()).ok());

  TensorManifest query = Manifest(8, std::byte{'x'});
  query.payload_digest = {};
  const std::array<std::uint64_t, 2> prefixes{4, 8};
  auto tokens = Tokens(query.token_count);
  query.tenant_id = "tenant-b";
  EXPECT_EQ(index.LongestPrefix(query, tokens, prefixes).status().code(), StatusCode::kNotFound);
  query = Manifest(8, std::byte{'x'});
  query.payload_digest = {};
  query.cache_format_version = 2;
  EXPECT_EQ(index.LongestPrefix(query, tokens, prefixes).status().code(), StatusCode::kNotFound);

  query = Manifest(8, std::byte{'x'});
  query.payload_digest = {};
  tokens[0] = 999;
  EXPECT_EQ(index.LongestPrefix(query, tokens, prefixes).status().code(),
            StatusCode::kInvalidArgument);
}

TEST(KvCacheMatchTest, ValidatesPrefixListAndCacheKey) {
  MatchIndex index;
  const TensorManifest entry = Manifest(4, std::byte{'x'});
  CacheKey wrong_key = CanonicalCacheKey(entry).value();
  wrong_key.digest[0] ^= std::byte{1};
  EXPECT_EQ(index.Insert(entry, wrong_key).code(), StatusCode::kInvalidArgument);
  ASSERT_TRUE(index.Insert(entry, CanonicalCacheKey(entry).value()).ok());

  TensorManifest query = Manifest(8, std::byte{'x'});
  query.payload_digest = {};
  const std::array<std::uint64_t, 2> duplicate{4, 4};
  const auto tokens = Tokens(query.token_count);
  EXPECT_EQ(index.LongestPrefix(query, tokens, duplicate).status().code(),
            StatusCode::kInvalidArgument);
}

TEST(KvCacheMatchTest, PhysicalVariantsHaveKeySpecificErase) {
  MatchIndex index;
  TensorManifest first = Manifest(8, std::byte{'x'});
  TensorManifest second = first;
  second.chunk_bytes = 128;
  second.chunk_count = 2;
  const auto first_key = CanonicalCacheKey(first);
  const auto second_key = CanonicalCacheKey(second);
  ASSERT_TRUE(first_key.ok());
  ASSERT_TRUE(second_key.ok());
  ASSERT_NE(first_key.value(), second_key.value());
  ASSERT_TRUE(index.Insert(first, first_key.value()).ok());
  ASSERT_TRUE(index.Insert(second, second_key.value()).ok());
  EXPECT_EQ(index.Size(), 2U);

  ASSERT_TRUE(index.Erase(second).ok());
  TensorManifest query = first;
  query.payload_digest = {};
  const auto tokens = Tokens(query.token_count);
  const auto match = index.Exact(query, tokens);
  ASSERT_TRUE(match.ok());
  EXPECT_EQ(match.value().key, first_key.value());
  EXPECT_EQ(index.Size(), 1U);
}

TEST(KvCacheMatchTest, EnforcesEntryPrefixChunkAndVariantLimits) {
  MatchIndexLimits limits;
  limits.max_entries = 1;
  limits.max_prefixes_per_query = 2;
  limits.max_chunks_per_result = 2;
  limits.max_variants_per_prefix = 1;
  MatchIndex index(limits);
  const TensorManifest too_many_chunks = Manifest(8, std::byte{'x'});
  EXPECT_EQ(index.Insert(too_many_chunks, CanonicalCacheKey(too_many_chunks).value()).code(),
            StatusCode::kLimitExceeded);

  TensorManifest entry = too_many_chunks;
  entry.chunk_bytes = 128;
  entry.chunk_count = 2;
  ASSERT_TRUE(index.Insert(entry, CanonicalCacheKey(entry).value()).ok());
  EXPECT_EQ(index.Insert(entry, CanonicalCacheKey(entry).value()).code(),
            StatusCode::kAlreadyExists);
  TensorManifest variant = entry;
  variant.chunk_bytes = 256;
  variant.chunk_count = 1;
  EXPECT_EQ(index.Insert(variant, CanonicalCacheKey(variant).value()).code(),
            StatusCode::kLimitExceeded);

  MatchIndexLimits variant_limits = limits;
  variant_limits.max_entries = 2;
  variant_limits.max_chunks_per_result = 4;
  MatchIndex variant_index(variant_limits);
  ASSERT_TRUE(variant_index.Insert(entry, CanonicalCacheKey(entry).value()).ok());
  EXPECT_EQ(variant_index.Insert(variant, CanonicalCacheKey(variant).value()).code(),
            StatusCode::kLimitExceeded);

  MatchIndexLimits entry_limits = limits;
  entry_limits.max_variants_per_prefix = 16;
  entry_limits.max_chunks_per_result = 8;
  MatchIndex entry_index(entry_limits);
  ASSERT_TRUE(entry_index.Insert(entry, CanonicalCacheKey(entry).value()).ok());
  const TensorManifest different_length = Manifest(4, std::byte{'x'});
  EXPECT_EQ(
      entry_index.Insert(different_length, CanonicalCacheKey(different_length).value()).code(),
      StatusCode::kLimitExceeded);

  TensorManifest query = entry;
  query.payload_digest = {};
  const std::array<std::uint64_t, 3> prefixes{2, 4, 8};
  const auto tokens = Tokens(query.token_count);
  EXPECT_EQ(index.LongestPrefix(query, tokens, prefixes).status().code(),
            StatusCode::kLimitExceeded);
  EXPECT_EQ(index.Exact(query, std::span<const std::uint32_t>{tokens.data(), 7}).status().code(),
            StatusCode::kInvalidArgument);
}

TEST(KvCacheMatchTest, EraseIsImmediatelyVisibleToConcurrentReaders) {
  MatchIndex index;
  const TensorManifest entry = Manifest(4, std::byte{'x'});
  ASSERT_TRUE(index.Insert(entry, CanonicalCacheKey(entry).value()).ok());
  TensorManifest query = entry;
  query.payload_digest = {};
  Result<MatchResult> lookup(Status{StatusCode::kInternal, "not run"});
  Status erased;
  const auto tokens = Tokens(query.token_count);
  std::thread reader([&] { lookup = index.Exact(query, tokens); });
  std::thread writer([&] { erased = index.Erase(entry); });
  reader.join();
  writer.join();
  EXPECT_TRUE(erased.ok());
  EXPECT_TRUE(lookup.ok() || lookup.status().code() == StatusCode::kNotFound);
  EXPECT_EQ(index.Exact(query, tokens).status().code(), StatusCode::kNotFound);
  EXPECT_EQ(index.Size(), 0U);
}

TEST(KvCacheMatchTest, ConcurrentReadersObserveOnlyCompleteRepeatedUpdates) {
  MatchIndex index;
  const TensorManifest entry = Manifest(4, std::byte{'x'});
  const CacheKey key = CanonicalCacheKey(entry).value();
  TensorManifest query = entry;
  query.payload_digest = {};
  std::atomic<bool> stop{false};
  std::atomic<bool> invalid{false};
  std::atomic<std::uint64_t> lookups{0};
  const auto tokens = Tokens(query.token_count);
  std::thread reader([&] {
    while (!stop.load(std::memory_order_acquire)) {
      const auto result = index.Exact(query, tokens);
      lookups.fetch_add(1, std::memory_order_relaxed);
      if (!result.ok() && result.status().code() != StatusCode::kNotFound) {
        invalid.store(true, std::memory_order_release);
      }
    }
  });
  for (int iteration = 0; iteration < 100; ++iteration) {
    ASSERT_TRUE(index.Insert(entry, key).ok());
    ASSERT_TRUE(index.Erase(entry).ok());
  }
  stop.store(true, std::memory_order_release);
  reader.join();
  EXPECT_FALSE(invalid.load(std::memory_order_acquire));
  EXPECT_GT(lookups.load(std::memory_order_relaxed), 0U);
}

}  // namespace
}  // namespace kvstore::kvcache
