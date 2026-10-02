#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "kvstore/common/byte_buffer.hpp"

namespace kvstore::kvcache {

inline constexpr std::uint16_t kManifestVersion = 1;
inline constexpr std::size_t kDigestBytes = 32;
inline constexpr std::uint32_t kChunkAlignmentBytes = 64;

using Digest = std::array<std::byte, kDigestBytes>;

enum class DType : std::uint8_t {
  kFloat16 = 1,
  kBFloat16 = 2,
  kFloat32 = 3,
};

enum class TensorLayout : std::uint8_t {
  kLayerMajor = 1,
  kBlockMajor = 2,
};

enum class TensorAxis : std::uint8_t {
  kKeyValue = 1,
  kLayer = 2,
  kToken = 3,
  kBlock = 4,
  kHead = 5,
  kHeadDimension = 6,
};

enum class KeyValuePacking : std::uint8_t {
  kPlanar = 1,
  kInterleaved = 2,
};

enum class DeviceKind : std::uint8_t {
  kCpu = 1,
  kCuda = 2,
};

enum class Compression : std::uint8_t {
  kNone = 0,
};

struct DeviceDescriptor {
  DeviceKind kind{DeviceKind::kCpu};
  std::uint32_t index{};

  friend bool operator==(const DeviceDescriptor&, const DeviceDescriptor&) = default;
};

struct TensorManifest {
  std::uint16_t version{kManifestVersion};
  std::string tenant_id;
  std::string model_id;
  std::string model_revision;
  std::string adapter_id;
  std::string adapter_revision;
  std::string tokenizer_revision;
  std::string cache_format;
  std::uint32_t cache_format_version{1};
  Digest token_digest{};
  std::uint64_t token_count{};
  std::uint32_t layer_begin{};
  std::uint32_t layer_count{};
  DType dtype{DType::kFloat16};
  std::vector<std::uint64_t> shape;
  std::vector<TensorAxis> axis_order;
  std::vector<std::uint64_t> strides_bytes;
  TensorLayout layout{TensorLayout::kLayerMajor};
  KeyValuePacking key_value_packing{KeyValuePacking::kPlanar};
  std::uint32_t block_tokens{};
  DeviceDescriptor device;
  std::uint32_t tensor_parallel_rank{};
  std::uint32_t tensor_parallel_size{1};
  std::uint32_t pipeline_parallel_rank{};
  std::uint32_t pipeline_parallel_size{1};
  std::uint64_t payload_bytes{};
  std::uint32_t chunk_bytes{};
  std::uint32_t chunk_alignment_bytes{kChunkAlignmentBytes};
  std::uint32_t chunk_count{};
  Compression compression{Compression::kNone};
  Digest payload_digest{};
  std::uint64_t created_at_ns{};
  std::uint64_t accessed_at_ns{};

  friend bool operator==(const TensorManifest&, const TensorManifest&) = default;
};

struct CacheKey {
  std::uint16_t version{kManifestVersion};
  Digest digest{};

  [[nodiscard]] std::string ToString() const;
  friend bool operator==(const CacheKey&, const CacheKey&) = default;
};

[[nodiscard]] Status ValidateManifest(const TensorManifest& manifest);
[[nodiscard]] Status ValidateManifestIdentity(const TensorManifest& manifest);
[[nodiscard]] Result<Bytes> EncodeCanonicalManifest(const TensorManifest& manifest);
[[nodiscard]] Result<TensorManifest> DecodeCanonicalManifest(ByteView bytes);
[[nodiscard]] Result<CacheKey> CanonicalCacheKey(const TensorManifest& manifest);
[[nodiscard]] Result<Digest> Sha256(ByteView data);
[[nodiscard]] Result<Digest> Sha256Parts(std::span<const ByteView> parts);
[[nodiscard]] Result<Digest> TokenDigest(std::span<const std::uint32_t> token_ids);
[[nodiscard]] std::string DigestHex(const Digest& digest);

}  // namespace kvstore::kvcache
