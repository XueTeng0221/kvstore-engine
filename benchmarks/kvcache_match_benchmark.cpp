#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

#include "kvstore/kvcache/match_index.hpp"

namespace {

using kvstore::Bytes;
using namespace kvstore::kvcache;

TensorManifest Manifest(std::uint64_t tokens) {
  TensorManifest manifest;
  manifest.tenant_id = "benchmark";
  manifest.model_id = "model";
  manifest.model_revision = "v1";
  manifest.tokenizer_revision = "v1";
  manifest.cache_format = "benchmark-v1";
  std::vector<std::uint32_t> ids(static_cast<std::size_t>(tokens), 1U);
  manifest.token_digest = TokenDigest(ids).value();
  manifest.token_count = tokens;
  manifest.layer_count = 1;
  manifest.shape = {2, 1, tokens, 1, 16};
  manifest.axis_order = {TensorAxis::kKeyValue, TensorAxis::kLayer, TensorAxis::kToken,
                         TensorAxis::kHead, TensorAxis::kHeadDimension};
  manifest.strides_bytes = {tokens * 32U, tokens * 32U, 32, 32, 2};
  manifest.payload_bytes = tokens * 64U;
  manifest.chunk_bytes = 2048;
  manifest.chunk_count = static_cast<std::uint32_t>((manifest.payload_bytes + 2047U) / 2048U);
  manifest.payload_digest = Sha256(Bytes(static_cast<std::size_t>(manifest.payload_bytes))).value();
  return manifest;
}

}  // namespace

int main() {
  constexpr std::uint64_t kIterations = 3000;
  for (const std::uint64_t query_tokens : {128U, 1024U, 4096U}) {
    for (const std::uint64_t entry_count : {2U, 16U, 64U}) {
      MatchIndex index;
      std::vector<std::uint64_t> prefixes;
      for (std::uint64_t entry = 1; entry <= entry_count; ++entry) {
        const std::uint64_t tokens = (query_tokens * entry) / (entry_count + 1U);
        TensorManifest manifest = Manifest(tokens);
        const auto key = CanonicalCacheKey(manifest);
        if (!key.ok() || !index.Insert(manifest, key.value()).ok()) {
          return 1;
        }
        prefixes.push_back(tokens);
      }
      TensorManifest query = Manifest(query_tokens);
      query.payload_digest = {};
      std::vector<std::uint32_t> hit_tokens(static_cast<std::size_t>(query_tokens), 1U);
      std::vector<std::uint32_t> miss_tokens(static_cast<std::size_t>(query_tokens), 2U);
      for (const std::uint64_t concurrency : {1U, 4U, 16U}) {
        for (const bool expect_hit : {true, false}) {
          query.token_digest = TokenDigest(expect_hit ? std::span<const std::uint32_t>{hit_tokens}
                                                      : std::span<const std::uint32_t>{miss_tokens})
                                   .value();
          const auto& selected_tokens = expect_hit ? hit_tokens : miss_tokens;
          static_cast<void>(index.LongestPrefix(query, selected_tokens, prefixes));
          std::atomic<bool> failed{false};
          std::vector<std::thread> workers;
          std::barrier start(static_cast<std::ptrdiff_t>(concurrency + 1U));
          for (std::uint64_t worker = 0; worker < concurrency; ++worker) {
            workers.emplace_back([&] {
              start.arrive_and_wait();
              for (std::uint64_t iteration = 0; iteration < kIterations / concurrency;
                   ++iteration) {
                const auto result = index.LongestPrefix(query, selected_tokens, prefixes);
                const bool unexpected =
                    expect_hit
                        ? !result.ok()
                        : (result.ok() || result.status().code() != kvstore::StatusCode::kNotFound);
                if (unexpected) {
                  failed.store(true, std::memory_order_relaxed);
                }
              }
            });
          }
          const auto begin = std::chrono::steady_clock::now();
          start.arrive_and_wait();
          for (std::thread& worker : workers) {
            worker.join();
          }
          if (failed.load(std::memory_order_relaxed)) {
            return 1;
          }
          const auto elapsed = std::chrono::steady_clock::now() - begin;
          const double elapsed_seconds = std::chrono::duration<double>(elapsed).count();
          const double lookups = static_cast<double>((kIterations / concurrency) * concurrency);
          std::cout << "tokens=" << query_tokens << " entries=" << index.Size()
                    << " concurrency=" << concurrency
                    << " result=" << (expect_hit ? "partial-hit" : "miss")
                    << " ns_per_lookup=" << (elapsed_seconds * 1e9 / lookups)
                    << " lookups_per_second=" << (lookups / elapsed_seconds) << '\n';
        }
      }
    }
  }
  return 0;
}
