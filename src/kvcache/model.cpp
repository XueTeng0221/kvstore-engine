#include "kvstore/kvcache/model.hpp"

#include <picosha2.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace kvstore::kvcache {
namespace {

constexpr std::size_t kMaxIdentityBytes = 4096;
constexpr std::size_t kMaxShapeDimensions = 16;
constexpr std::uint32_t kMaxChunkBytes = 64U * 1024U * 1024U;

void AppendU8(Bytes& output, std::uint8_t value) {
  output.push_back(static_cast<std::byte>(value));
}

void AppendU16(Bytes& output, std::uint16_t value) {
  AppendU8(output, static_cast<std::uint8_t>(value >> 8U));
  AppendU8(output, static_cast<std::uint8_t>(value));
}

void AppendU32(Bytes& output, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    AppendU8(output, static_cast<std::uint8_t>(value >> static_cast<unsigned int>(shift)));
  }
}

void AppendU64(Bytes& output, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    AppendU8(output, static_cast<std::uint8_t>(value >> static_cast<unsigned int>(shift)));
  }
}

void AppendString(Bytes& output, std::string_view value) {
  AppendU32(output, static_cast<std::uint32_t>(value.size()));
  for (const char byte : value) {
    AppendU8(output, static_cast<std::uint8_t>(static_cast<unsigned char>(byte)));
  }
}

void AppendDigest(Bytes& output, const Digest& digest) {
  output.insert(output.end(), digest.begin(), digest.end());
}

bool IsZeroDigest(const Digest& digest) {
  for (const std::byte byte : digest) {
    if (byte != std::byte{0}) {
      return false;
    }
  }
  return true;
}

Result<std::size_t> DTypeBytes(DType dtype) {
  switch (dtype) {
    case DType::kFloat16:
    case DType::kBFloat16:
      return std::size_t{2};
    case DType::kFloat32:
      return std::size_t{4};
  }
  return Status{StatusCode::kInvalidArgument, "unsupported tensor dtype"};
}

bool ValidLayout(TensorLayout layout) {
  return layout == TensorLayout::kLayerMajor || layout == TensorLayout::kBlockMajor;
}

bool ValidDevice(DeviceKind kind) { return kind == DeviceKind::kCpu || kind == DeviceKind::kCuda; }

bool ValidPacking(KeyValuePacking packing) {
  return packing == KeyValuePacking::kPlanar || packing == KeyValuePacking::kInterleaved;
}

Result<std::size_t> AxisIndex(const TensorManifest& manifest, TensorAxis axis) {
  const auto iterator = std::ranges::find(manifest.axis_order, axis);
  if (iterator == manifest.axis_order.end()) {
    return Status{StatusCode::kInvalidArgument, "required tensor axis is missing"};
  }
  return static_cast<std::size_t>(std::distance(manifest.axis_order.begin(), iterator));
}

bool HasAxisOrder(const TensorManifest& manifest, std::initializer_list<TensorAxis> expected) {
  return std::ranges::equal(manifest.axis_order, expected);
}

template <typename T>
bool ReadBig(ByteView bytes, std::size_t& offset, T& value) {
  if (bytes.size() - offset < sizeof(T)) return false;
  value = 0;
  for (std::size_t index = 0; index < sizeof(T); ++index)
    value = static_cast<T>((value << 8U) |
                            std::to_integer<std::uint8_t>(bytes[offset++]));
  return true;
}

bool ReadText(ByteView bytes, std::size_t& offset, std::string& value) {
  std::uint32_t size = 0;
  if (!ReadBig(bytes, offset, size) || size > kMaxIdentityBytes ||
      size > bytes.size() - offset) return false;
  value.assign(reinterpret_cast<const char*>(bytes.data() + offset), size);
  offset += size;
  return true;
}

}  // namespace

Status ValidateManifestIdentity(const TensorManifest& manifest) {
  if (manifest.version != kManifestVersion) {
    return {StatusCode::kUnsupported, "unsupported tensor manifest version"};
  }
  const auto valid_identity = [](const std::string& value) {
    return !value.empty() && value.size() <= kMaxIdentityBytes;
  };
  if (!valid_identity(manifest.tenant_id) || !valid_identity(manifest.model_id) ||
      !valid_identity(manifest.model_revision) || !valid_identity(manifest.tokenizer_revision) ||
      !valid_identity(manifest.cache_format) || manifest.cache_format_version == 0) {
    return {StatusCode::kInvalidArgument, "required cache identity field is empty or too large"};
  }
  if (manifest.adapter_id.empty() != manifest.adapter_revision.empty() ||
      manifest.adapter_id.size() > kMaxIdentityBytes ||
      manifest.adapter_revision.size() > kMaxIdentityBytes) {
    return {StatusCode::kInvalidArgument, "adapter identity must be absent or complete"};
  }
  if (manifest.token_count == 0 || IsZeroDigest(manifest.token_digest)) {
    return {StatusCode::kInvalidArgument, "token identity is incomplete"};
  }
  if (manifest.layer_count == 0 ||
      manifest.layer_begin > std::numeric_limits<std::uint32_t>::max() - manifest.layer_count) {
    return {StatusCode::kInvalidArgument, "invalid layer range"};
  }
  const auto dtype_bytes = DTypeBytes(manifest.dtype);
  if (!dtype_bytes.ok()) {
    return dtype_bytes.status();
  }
  if (manifest.shape.empty() || manifest.shape.size() > kMaxShapeDimensions ||
      manifest.axis_order.size() != manifest.shape.size() ||
      manifest.strides_bytes.size() != manifest.shape.size() || !ValidLayout(manifest.layout) ||
      !ValidPacking(manifest.key_value_packing) || !ValidDevice(manifest.device.kind)) {
    return {StatusCode::kInvalidArgument, "invalid tensor shape, layout, or device"};
  }
  std::array<bool, 7> seen_axes{};
  for (const TensorAxis axis : manifest.axis_order) {
    const auto value = static_cast<std::uint8_t>(axis);
    if (value == 0 || value >= seen_axes.size() || seen_axes[value]) {
      return {StatusCode::kInvalidArgument, "tensor axes must be recognized and unique"};
    }
    seen_axes[value] = true;
  }
  const auto key_value_axis = AxisIndex(manifest, TensorAxis::kKeyValue);
  const auto layer_axis = AxisIndex(manifest, TensorAxis::kLayer);
  const auto token_axis = AxisIndex(manifest, TensorAxis::kToken);
  const auto head_axis = AxisIndex(manifest, TensorAxis::kHead);
  const auto head_dimension_axis = AxisIndex(manifest, TensorAxis::kHeadDimension);
  if (!key_value_axis.ok() || !layer_axis.ok() || !token_axis.ok() || !head_axis.ok() ||
      !head_dimension_axis.ok() || manifest.shape[key_value_axis.value()] != 2 ||
      manifest.shape[layer_axis.value()] != manifest.layer_count) {
    return {StatusCode::kInvalidArgument, "tensor axes do not describe the manifest range"};
  }
  const auto block_axis = AxisIndex(manifest, TensorAxis::kBlock);
  if (manifest.layout == TensorLayout::kLayerMajor) {
    if (block_axis.ok() || manifest.block_tokens != 0 ||
        manifest.key_value_packing != KeyValuePacking::kPlanar ||
        !HasAxisOrder(manifest, {TensorAxis::kKeyValue, TensorAxis::kLayer, TensorAxis::kToken,
                                 TensorAxis::kHead, TensorAxis::kHeadDimension}) ||
        manifest.shape[token_axis.value()] != manifest.token_count) {
      return {StatusCode::kInvalidArgument, "invalid layer-major token layout"};
    }
  } else {
    const bool valid_planar =
        manifest.key_value_packing == KeyValuePacking::kPlanar &&
        HasAxisOrder(manifest, {TensorAxis::kLayer, TensorAxis::kKeyValue, TensorAxis::kBlock,
                                TensorAxis::kToken, TensorAxis::kHead, TensorAxis::kHeadDimension});
    const bool valid_interleaved =
        manifest.key_value_packing == KeyValuePacking::kInterleaved &&
        HasAxisOrder(manifest,
                     {TensorAxis::kLayer, TensorAxis::kBlock, TensorAxis::kToken,
                      TensorAxis::kKeyValue, TensorAxis::kHead, TensorAxis::kHeadDimension});
    if (!block_axis.ok() || (!valid_planar && !valid_interleaved) || manifest.block_tokens == 0 ||
        manifest.shape[token_axis.value()] != manifest.block_tokens ||
        manifest.shape[block_axis.value()] >
            std::numeric_limits<std::uint64_t>::max() / manifest.block_tokens) {
      return {StatusCode::kInvalidArgument, "invalid block-major token layout"};
    }
    const std::uint64_t token_capacity = manifest.shape[block_axis.value()] * manifest.block_tokens;
    if (manifest.token_count > token_capacity ||
        manifest.token_count <= token_capacity - manifest.block_tokens) {
      return {StatusCode::kInvalidArgument, "block-major token count is outside final block"};
    }
  }
  if (manifest.tensor_parallel_size == 0 ||
      manifest.tensor_parallel_rank >= manifest.tensor_parallel_size ||
      manifest.pipeline_parallel_size == 0 ||
      manifest.pipeline_parallel_rank >= manifest.pipeline_parallel_size) {
    return {StatusCode::kInvalidArgument, "invalid parallel topology"};
  }
  std::uint64_t expected_bytes = static_cast<std::uint64_t>(dtype_bytes.value());
  for (std::size_t index = manifest.shape.size(); index > 0; --index) {
    const std::size_t axis = index - 1;
    const std::uint64_t dimension = manifest.shape[axis];
    if (dimension == 0 || manifest.strides_bytes[axis] != expected_bytes ||
        expected_bytes > std::numeric_limits<std::uint64_t>::max() / dimension) {
      return {StatusCode::kLimitExceeded, "tensor shape byte size overflows"};
    }
    expected_bytes *= dimension;
  }
  if (manifest.payload_bytes == 0 || manifest.payload_bytes != expected_bytes) {
    return {StatusCode::kInvalidArgument, "payload size does not match tensor shape"};
  }
  if (manifest.chunk_bytes == 0 || manifest.chunk_bytes > kMaxChunkBytes ||
      manifest.chunk_alignment_bytes != kChunkAlignmentBytes ||
      manifest.chunk_bytes % manifest.chunk_alignment_bytes != 0) {
    return {StatusCode::kInvalidArgument, "invalid chunk size or alignment"};
  }
  const std::uint64_t expected_chunks = 1U + ((manifest.payload_bytes - 1U) / manifest.chunk_bytes);
  if (expected_chunks > std::numeric_limits<std::uint32_t>::max() ||
      manifest.chunk_count != expected_chunks) {
    return {StatusCode::kInvalidArgument, "chunk count does not match payload size"};
  }
  if (manifest.compression != Compression::kNone) {
    return {StatusCode::kUnsupported, "compression is not supported by manifest version 1"};
  }
  return Status::Ok();
}

Status ValidateManifest(const TensorManifest& manifest) {
  const Status identity = ValidateManifestIdentity(manifest);
  if (!identity.ok()) {
    return identity;
  }
  if (IsZeroDigest(manifest.payload_digest)) {
    return {StatusCode::kInvalidArgument, "payload digest is missing"};
  }
  return Status::Ok();
}

Result<Bytes> EncodeCanonicalManifest(const TensorManifest& manifest) {
  const Status status = ValidateManifestIdentity(manifest);
  if (!status.ok()) {
    return status;
  }

  Bytes output;
  try {
    output.reserve(256);
    for (const char byte : std::string_view{"KVC1"}) {
      AppendU8(output, static_cast<std::uint8_t>(static_cast<unsigned char>(byte)));
    }
    AppendU16(output, manifest.version);
    AppendString(output, manifest.tenant_id);
    AppendString(output, manifest.model_id);
    AppendString(output, manifest.model_revision);
    AppendString(output, manifest.adapter_id);
    AppendString(output, manifest.adapter_revision);
    AppendString(output, manifest.tokenizer_revision);
    AppendString(output, manifest.cache_format);
    AppendU32(output, manifest.cache_format_version);
    AppendDigest(output, manifest.token_digest);
    AppendU64(output, manifest.token_count);
    AppendU32(output, manifest.layer_begin);
    AppendU32(output, manifest.layer_count);
    AppendU8(output, static_cast<std::uint8_t>(manifest.dtype));
    AppendU32(output, static_cast<std::uint32_t>(manifest.shape.size()));
    for (std::size_t index = 0; index < manifest.shape.size(); ++index) {
      AppendU8(output, static_cast<std::uint8_t>(manifest.axis_order[index]));
      AppendU64(output, manifest.shape[index]);
      AppendU64(output, manifest.strides_bytes[index]);
    }
    AppendU8(output, static_cast<std::uint8_t>(manifest.layout));
    AppendU8(output, static_cast<std::uint8_t>(manifest.key_value_packing));
    AppendU32(output, manifest.block_tokens);
    AppendU8(output, static_cast<std::uint8_t>(manifest.device.kind));
    AppendU32(output, manifest.device.index);
    AppendU32(output, manifest.tensor_parallel_rank);
    AppendU32(output, manifest.tensor_parallel_size);
    AppendU32(output, manifest.pipeline_parallel_rank);
    AppendU32(output, manifest.pipeline_parallel_size);
    AppendU64(output, manifest.payload_bytes);
    AppendU32(output, manifest.chunk_bytes);
    AppendU32(output, manifest.chunk_alignment_bytes);
    AppendU32(output, manifest.chunk_count);
    AppendU8(output, static_cast<std::uint8_t>(manifest.compression));
  } catch (const std::bad_alloc&) {
    return Status{StatusCode::kLimitExceeded, "unable to allocate canonical manifest"};
  }
  return output;
}

Result<TensorManifest> DecodeCanonicalManifest(ByteView bytes) try {
  TensorManifest manifest;
  std::size_t offset = 0;
  std::uint8_t tag = 0, dtype = 0, layout = 0, packing = 0, device = 0, compression = 0;
  std::uint16_t version = 0;
  std::uint32_t count = 0;
  if (bytes.size() < 6 || !ReadBig(bytes, offset, tag) || tag != 'K' ||
      !ReadBig(bytes, offset, tag) || tag != 'V' || !ReadBig(bytes, offset, tag) || tag != 'C' ||
      !ReadBig(bytes, offset, tag) || tag != '1' || !ReadBig(bytes, offset, version) ||
      version != kManifestVersion || !ReadText(bytes, offset, manifest.tenant_id) ||
      !ReadText(bytes, offset, manifest.model_id) || !ReadText(bytes, offset, manifest.model_revision) ||
      !ReadText(bytes, offset, manifest.adapter_id) || !ReadText(bytes, offset, manifest.adapter_revision) ||
      !ReadText(bytes, offset, manifest.tokenizer_revision) ||
      !ReadText(bytes, offset, manifest.cache_format) || !ReadBig(bytes, offset, manifest.cache_format_version))
    return Status{StatusCode::kCorruption, "invalid canonical manifest header"};
  manifest.version = version;
  if (bytes.size() - offset < kDigestBytes)
    return Status{StatusCode::kCorruption, "invalid canonical manifest fields"};
  std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), kDigestBytes,
              manifest.token_digest.begin());
  offset += kDigestBytes;
  if (!ReadBig(bytes, offset, manifest.token_count) ||
      !ReadBig(bytes, offset, manifest.layer_begin) || !ReadBig(bytes, offset, manifest.layer_count) ||
      !ReadBig(bytes, offset, dtype) || !ReadBig(bytes, offset, count) || count > kMaxShapeDimensions)
    return Status{StatusCode::kCorruption, "invalid canonical manifest fields"};
  manifest.dtype = static_cast<DType>(dtype);
  manifest.shape.reserve(count); manifest.axis_order.reserve(count); manifest.strides_bytes.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    std::uint8_t axis = 0; std::uint64_t shape = 0, stride = 0;
    if (!ReadBig(bytes, offset, axis) || !ReadBig(bytes, offset, shape) ||
        !ReadBig(bytes, offset, stride))
      return Status{StatusCode::kCorruption, "truncated canonical shape"};
    manifest.axis_order.push_back(static_cast<TensorAxis>(axis));
    manifest.shape.push_back(shape); manifest.strides_bytes.push_back(stride);
  }
  if (!ReadBig(bytes, offset, layout) || !ReadBig(bytes, offset, packing) ||
      !ReadBig(bytes, offset, manifest.block_tokens) || !ReadBig(bytes, offset, device) ||
      !ReadBig(bytes, offset, manifest.device.index) ||
      !ReadBig(bytes, offset, manifest.tensor_parallel_rank) ||
      !ReadBig(bytes, offset, manifest.tensor_parallel_size) ||
      !ReadBig(bytes, offset, manifest.pipeline_parallel_rank) ||
      !ReadBig(bytes, offset, manifest.pipeline_parallel_size) ||
      !ReadBig(bytes, offset, manifest.payload_bytes) || !ReadBig(bytes, offset, manifest.chunk_bytes) ||
      !ReadBig(bytes, offset, manifest.chunk_alignment_bytes) ||
      !ReadBig(bytes, offset, manifest.chunk_count) || !ReadBig(bytes, offset, compression) ||
      offset != bytes.size())
    return Status{StatusCode::kCorruption, "truncated canonical manifest metadata"};
  manifest.layout = static_cast<TensorLayout>(layout);
  manifest.key_value_packing = static_cast<KeyValuePacking>(packing);
  manifest.device.kind = static_cast<DeviceKind>(device);
  manifest.compression = static_cast<Compression>(compression);
  if (auto status = ValidateManifestIdentity(manifest); !status.ok()) return status;
  return manifest;
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "canonical manifest allocation"};
}

Result<Digest> Sha256Parts(std::span<const ByteView> parts) try {
  picosha2::hash256_one_by_one hasher;
  std::array<unsigned char, 4096> input{};
  for (const ByteView part : parts) {
    for (std::size_t offset = 0; offset < part.size();) {
      const std::size_t size = std::min(input.size(), part.size() - offset);
      for (std::size_t index = 0; index < size; ++index) {
        input[index] = std::to_integer<unsigned char>(part[offset + index]);
      }
      hasher.process(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(size));
      offset += size;
    }
  }
  hasher.finish();
  Digest digest{};
  std::array<unsigned char, kDigestBytes> output{};
  hasher.get_hash_bytes(output.begin(), output.end());
  for (std::size_t index = 0; index < digest.size(); ++index) {
    digest[index] = static_cast<std::byte>(output[index]);
  }
  return digest;
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "unable to allocate SHA-256 state"};
}

Result<Digest> Sha256(ByteView data) { return Sha256Parts(std::span<const ByteView>{&data, 1}); }

Result<Digest> TokenDigest(std::span<const std::uint32_t> token_ids) try {
  picosha2::hash256_one_by_one hasher;
  for (const std::uint32_t token_id : token_ids) {
    std::array<unsigned char, 4> encoded{
        static_cast<unsigned char>(token_id >> 24U), static_cast<unsigned char>(token_id >> 16U),
        static_cast<unsigned char>(token_id >> 8U), static_cast<unsigned char>(token_id)};
    hasher.process(encoded.begin(), encoded.end());
  }
  hasher.finish();
  std::array<unsigned char, kDigestBytes> output{};
  hasher.get_hash_bytes(output.begin(), output.end());
  Digest digest{};
  for (std::size_t index = 0; index < digest.size(); ++index) {
    digest[index] = static_cast<std::byte>(output[index]);
  }
  return digest;
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "unable to allocate token digest state"};
}

std::string DigestHex(const Digest& digest) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string output;
  output.reserve(digest.size() * 2U);
  for (const std::byte byte : digest) {
    const auto value = std::to_integer<std::uint8_t>(byte);
    output.push_back(kHex[value >> 4U]);
    output.push_back(kHex[value & 0x0fU]);
  }
  return output;
}

std::string CacheKey::ToString() const {
  return "kvc" + std::to_string(version) + ":" + DigestHex(digest);
}

Result<CacheKey> CanonicalCacheKey(const TensorManifest& manifest) {
  auto encoded = EncodeCanonicalManifest(manifest);
  if (!encoded.ok()) {
    return encoded.status();
  }
  auto digest = Sha256(encoded.value());
  if (!digest.ok()) {
    return digest.status();
  }
  return CacheKey{manifest.version, digest.value()};
}

}  // namespace kvstore::kvcache
