#pragma once

#include <cstdint>
#include <map>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "kvstore/kvcache/model.hpp"

namespace kvstore::kvcache {

struct TokenRange {
  std::uint64_t begin{};
  std::uint64_t end{};

  friend bool operator==(const TokenRange&, const TokenRange&) = default;
};

struct MatchResult {
  CacheKey key;
  TensorManifest source_manifest;
  std::uint64_t hit_tokens{};
  std::uint32_t layer_begin{};
  std::uint32_t layer_count{};
  std::vector<std::uint32_t> chunk_indices;
  std::vector<TokenRange> missing_tokens;
};

struct MatchIndexLimits {
  std::size_t max_entries{1'000'000};
  std::size_t max_prefixes_per_query{4096};
  std::uint64_t max_tokens_per_query{131072};
  std::uint32_t max_chunks_per_result{4096};
  std::size_t max_variants_per_prefix{16};
};

class MatchIndex {
 public:
  explicit MatchIndex(MatchIndexLimits limits = {}) : limits_(limits) {}

  [[nodiscard]] Status Insert(const TensorManifest& manifest, const CacheKey& key);
  [[nodiscard]] Status Erase(const TensorManifest& manifest);
  [[nodiscard]] Result<MatchResult> Exact(const TensorManifest& query,
                                          std::span<const std::uint32_t> token_ids) const;
  [[nodiscard]] Result<MatchResult> LongestPrefix(
      const TensorManifest& query, std::span<const std::uint32_t> token_ids,
      std::span<const std::uint64_t> prefix_lengths) const;
  [[nodiscard]] std::size_t Size() const;

 private:
  struct Entry {
    TensorManifest manifest;
    CacheKey key;
    Bytes compatibility;
  };

  using LengthIndex = std::map<std::uint64_t, std::vector<Entry>, std::greater<>>;

  MatchIndexLimits limits_;
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, LengthIndex> partitions_;
  std::size_t size_{};
};

[[nodiscard]] Result<Bytes> EncodeTensorCompatibility(const TensorManifest& manifest);

}  // namespace kvstore::kvcache
