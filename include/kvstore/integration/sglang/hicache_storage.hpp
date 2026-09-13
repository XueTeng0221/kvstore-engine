#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "kvstore/integration/protocol.hpp"

namespace kvstore::integration::sglang {

inline constexpr std::string_view kSupportedSglang = "0.5.19";
inline constexpr std::string_view kSupportedModel = "Qwen2.5-0.5B";

struct RadixMetadata {
  std::string model_revision;
  std::string tokenizer_revision;
  std::uint32_t layer_begin{};
  std::uint32_t layer_count{};
  std::uint32_t head_count{};
  std::uint32_t head_dimension{};
  std::uint32_t block_tokens{};
  kvcache::DType dtype{kvcache::DType::kFloat16};
  kvcache::DeviceDescriptor device;
};

struct PrefixQuery {
  RadixMetadata metadata;
  std::span<const std::uint32_t> token_ids;
  std::span<const std::uint64_t> prefix_lengths;
  bool exact{};
};

[[nodiscard]] Result<kvcache::TensorManifest> CanonicalManifest(
    const PrefixQuery&, std::string tenant, std::string model);

// This mirrors SGLang 0.5.19 dynamic HiCacheStorage's operation boundary. It owns
// no framework objects and uses P8.1 serialized request/response bytes.
class HiCacheStorage {
 public:
  HiCacheStorage(Session& session, std::string tenant, std::string model);
  [[nodiscard]] Result<v1::Response> Lookup(const PrefixQuery&, std::uint64_t deadline = 0,
                                            bool cancelled = false);
  [[nodiscard]] Result<v1::Response> Publish(const kvcache::TensorManifest&, std::span<const Bytes>);
  [[nodiscard]] Status Release(std::uint64_t lease_id);
  [[nodiscard]] Status Abort(std::uint64_t reservation_id);

 private:
  [[nodiscard]] Result<v1::Response> Call(v1::Request&);
  Session& session_;
  std::string tenant_, model_;
  std::uint64_t sequence_{1};
};
}  // namespace kvstore::integration::sglang
