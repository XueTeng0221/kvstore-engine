#include "kvstore/kvcache/match_index.hpp"

#include <picosha2.h>

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

namespace kvstore::kvcache {
namespace {

void AppendU32(Bytes& output, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    output.push_back(static_cast<std::byte>(value >> static_cast<unsigned int>(shift)));
  }
}

void AppendU64(Bytes& output, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    output.push_back(static_cast<std::byte>(value >> static_cast<unsigned int>(shift)));
  }
}

void AppendString(Bytes& output, const std::string& value) {
  AppendU32(output, static_cast<std::uint32_t>(value.size()));
  for (const char byte : value) {
    output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
  }
}

Result<std::string> PartitionKey(ByteView compatibility) {
  auto digest = Sha256(compatibility);
  if (!digest.ok()) {
    return digest.status();
  }
  return DigestHex(digest.value());
}

bool LessKey(const CacheKey& left, const CacheKey& right) {
  if (left.version != right.version) {
    return left.version < right.version;
  }
  return std::ranges::lexicographical_compare(left.digest, right.digest);
}

Digest FinishedDigest(picosha2::hash256_one_by_one hasher) {
  hasher.finish();
  std::array<unsigned char, kDigestBytes> output{};
  hasher.get_hash_bytes(output.begin(), output.end());
  Digest digest{};
  for (std::size_t index = 0; index < digest.size(); ++index) {
    digest[index] = static_cast<std::byte>(output[index]);
  }
  return digest;
}

Result<std::unordered_map<std::uint64_t, Digest>> ComputePrefixDigests(
    std::span<const std::uint32_t> token_ids, std::span<const std::uint64_t> prefix_lengths,
    const Digest& expected_full_digest) try {
  std::unordered_map<std::uint64_t, Digest> result;
  result.reserve(prefix_lengths.size());
  picosha2::hash256_one_by_one hasher;
  std::size_t prefix_index = 0;
  for (std::size_t index = 0; index < token_ids.size(); ++index) {
    const std::uint32_t token_id = token_ids[index];
    const std::array<unsigned char, 4> encoded{
        static_cast<unsigned char>(token_id >> 24U), static_cast<unsigned char>(token_id >> 16U),
        static_cast<unsigned char>(token_id >> 8U), static_cast<unsigned char>(token_id)};
    hasher.process(encoded.begin(), encoded.end());
    const std::uint64_t consumed = static_cast<std::uint64_t>(index) + 1U;
    if (prefix_index < prefix_lengths.size() && prefix_lengths[prefix_index] == consumed) {
      result.emplace(consumed, FinishedDigest(hasher));
      ++prefix_index;
    }
  }
  if (FinishedDigest(hasher) != expected_full_digest) {
    return Status{StatusCode::kInvalidArgument, "token IDs do not match query token digest"};
  }
  return result;
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "unable to allocate cumulative token digests"};
}

std::vector<std::uint32_t> ChunkIndices(const TensorManifest& manifest) {
  std::vector<std::uint32_t> result;
  result.reserve(manifest.chunk_count);
  for (std::uint32_t index = 0; index < manifest.chunk_count; ++index) {
    result.push_back(index);
  }
  return result;
}

MatchResult MakeResult(const TensorManifest& manifest, const CacheKey& key,
                       std::uint64_t query_tokens) {
  MatchResult result{
      key, manifest, manifest.token_count, manifest.layer_begin, manifest.layer_count,
      ChunkIndices(manifest), {}};
  if (manifest.token_count < query_tokens) {
    result.missing_tokens.push_back({manifest.token_count, query_tokens});
  }
  return result;
}

}  // namespace

Result<Bytes> EncodeTensorCompatibility(const TensorManifest& manifest) {
  const Status status = ValidateManifestIdentity(manifest);
  if (!status.ok()) {
    return status;
  }
  try {
    Bytes output;
    output.reserve(256);
    AppendString(output, "KVC-COMPAT-1");
    AppendU32(output, manifest.version);
    AppendString(output, manifest.tenant_id);
    AppendString(output, manifest.model_id);
    AppendString(output, manifest.model_revision);
    AppendString(output, manifest.adapter_id);
    AppendString(output, manifest.adapter_revision);
    AppendString(output, manifest.tokenizer_revision);
    AppendString(output, manifest.cache_format);
    AppendU32(output, manifest.cache_format_version);
    AppendU32(output, manifest.layer_begin);
    AppendU32(output, manifest.layer_count);
    output.push_back(static_cast<std::byte>(manifest.dtype));
    output.push_back(static_cast<std::byte>(manifest.layout));
    output.push_back(static_cast<std::byte>(manifest.key_value_packing));
    AppendU32(output, manifest.block_tokens);
    output.push_back(static_cast<std::byte>(manifest.device.kind));
    AppendU32(output, manifest.device.index);
    AppendU32(output, manifest.tensor_parallel_rank);
    AppendU32(output, manifest.tensor_parallel_size);
    AppendU32(output, manifest.pipeline_parallel_rank);
    AppendU32(output, manifest.pipeline_parallel_size);
    AppendU32(output, static_cast<std::uint32_t>(manifest.shape.size()));
    for (std::size_t index = 0; index < manifest.shape.size(); ++index) {
      output.push_back(static_cast<std::byte>(manifest.axis_order[index]));
      const TensorAxis axis = manifest.axis_order[index];
      if (axis != TensorAxis::kToken && axis != TensorAxis::kBlock) {
        AppendU64(output, manifest.shape[index]);
      }
    }
    return output;
  } catch (const std::bad_alloc&) {
    return Status{StatusCode::kLimitExceeded, "unable to allocate compatibility descriptor"};
  }
}

Status MatchIndex::Insert(const TensorManifest& manifest, const CacheKey& key) try {
  const Status status = ValidateManifest(manifest);
  if (!status.ok()) {
    return status;
  }
  auto expected_key = CanonicalCacheKey(manifest);
  if (!expected_key.ok()) {
    return expected_key.status();
  }
  if (expected_key.value() != key) {
    return {StatusCode::kInvalidArgument, "cache key does not match manifest"};
  }
  auto compatibility = EncodeTensorCompatibility(manifest);
  if (!compatibility.ok()) {
    return compatibility.status();
  }
  auto partition_key = PartitionKey(compatibility.value());
  if (!partition_key.ok()) {
    return partition_key.status();
  }

  std::unique_lock lock(mutex_);
  if (limits_.max_entries == 0 || limits_.max_prefixes_per_query == 0 ||
      limits_.max_tokens_per_query == 0 || limits_.max_chunks_per_result == 0 ||
      limits_.max_variants_per_prefix == 0) {
    return {StatusCode::kInvalidArgument, "match index limits must be positive"};
  }
  const auto existing_partition = partitions_.find(partition_key.value());
  if (existing_partition != partitions_.end()) {
    const auto existing_length = existing_partition->second.find(manifest.token_count);
    if (existing_length != existing_partition->second.end() &&
        std::ranges::any_of(existing_length->second,
                            [&](const Entry& entry) { return entry.key == key; })) {
      return {StatusCode::kAlreadyExists, "match index entry already exists"};
    }
  }
  if (size_ >= limits_.max_entries || manifest.chunk_count > limits_.max_chunks_per_result) {
    return {StatusCode::kLimitExceeded, "match index capacity exceeded"};
  }
  auto& entries = partitions_[partition_key.value()][manifest.token_count];
  if (entries.size() >= limits_.max_variants_per_prefix) {
    return {StatusCode::kLimitExceeded, "physical variants per prefix exceeded"};
  }
  entries.push_back({manifest, key, std::move(compatibility.value())});
  ++size_;
  return Status::Ok();
} catch (const std::bad_alloc&) {
  return {StatusCode::kLimitExceeded, "unable to allocate match index entry"};
}

Status MatchIndex::Erase(const TensorManifest& manifest) try {
  auto expected_key = CanonicalCacheKey(manifest);
  if (!expected_key.ok()) {
    return expected_key.status();
  }
  auto compatibility = EncodeTensorCompatibility(manifest);
  if (!compatibility.ok()) {
    return compatibility.status();
  }
  auto partition_key = PartitionKey(compatibility.value());
  if (!partition_key.ok()) {
    return partition_key.status();
  }
  std::unique_lock lock(mutex_);
  auto partition = partitions_.find(partition_key.value());
  if (partition == partitions_.end()) {
    return {StatusCode::kNotFound, "match index entry does not exist"};
  }
  auto length = partition->second.find(manifest.token_count);
  if (length == partition->second.end()) {
    return {StatusCode::kNotFound, "match index entry does not exist"};
  }
  auto& entries = length->second;
  const auto entry = std::ranges::find_if(
      entries, [&](const Entry& candidate) { return candidate.key == expected_key.value(); });
  if (entry == entries.end()) {
    return {StatusCode::kNotFound, "match index entry does not exist"};
  }
  entries.erase(entry);
  --size_;
  if (entries.empty()) {
    partition->second.erase(length);
  }
  if (partition->second.empty()) {
    partitions_.erase(partition);
  }
  return Status::Ok();
} catch (const std::bad_alloc&) {
  return {StatusCode::kLimitExceeded, "unable to allocate match erase key"};
}

Result<MatchResult> MatchIndex::Exact(const TensorManifest& query,
                                      std::span<const std::uint32_t> token_ids) const {
  const std::uint64_t exact = query.token_count;
  auto result = LongestPrefix(query, token_ids, std::span<const std::uint64_t>{&exact, 1});
  if (!result.ok()) {
    return result.status();
  }
  if (result.value().hit_tokens != query.token_count) {
    return Status{StatusCode::kNotFound, "exact cache match does not exist"};
  }
  return result;
}

Result<MatchResult> MatchIndex::LongestPrefix(const TensorManifest& query,
                                              std::span<const std::uint32_t> token_ids,
                                              std::span<const std::uint64_t> prefix_lengths) const
    try {
  const Status status = ValidateManifestIdentity(query);
  if (!status.ok()) {
    return status;
  }
  auto compatibility = EncodeTensorCompatibility(query);
  if (!compatibility.ok()) {
    return compatibility.status();
  }
  auto partition_key = PartitionKey(compatibility.value());
  if (!partition_key.ok()) {
    return partition_key.status();
  }

  if (token_ids.size() != query.token_count) {
    return Status{StatusCode::kInvalidArgument, "token count does not match query manifest"};
  }
  if (token_ids.size() > limits_.max_tokens_per_query) {
    return Status{StatusCode::kLimitExceeded, "token query size exceeds configured limit"};
  }
  if (prefix_lengths.empty() || prefix_lengths.size() > limits_.max_prefixes_per_query) {
    return Status{StatusCode::kLimitExceeded, "prefix query size exceeds configured limit"};
  }
  std::uint64_t previous = 0;
  for (const std::uint64_t length : prefix_lengths) {
    if (length <= previous || length > query.token_count) {
      return Status{StatusCode::kInvalidArgument, "prefix lengths must be strictly increasing"};
    }
    previous = length;
  }
  auto prefix_by_length = ComputePrefixDigests(token_ids, prefix_lengths, query.token_digest);
  if (!prefix_by_length.ok()) {
    return prefix_by_length.status();
  }
  std::shared_lock lock(mutex_);
  const auto partition = partitions_.find(partition_key.value());
  if (partition == partitions_.end()) {
    return Status{StatusCode::kNotFound, "compatible cache partition does not exist"};
  }
  for (const auto& [token_count, entries] : partition->second) {
    const auto prefix = prefix_by_length.value().find(token_count);
    if (prefix == prefix_by_length.value().end()) {
      continue;
    }
    const Entry* selected = nullptr;
    for (const Entry& entry : entries) {
      if (entry.compatibility == compatibility.value() &&
          entry.manifest.token_digest == prefix->second &&
          (selected == nullptr || LessKey(entry.key, selected->key))) {
        selected = &entry;
      }
    }
    if (selected != nullptr) {
      return MakeResult(selected->manifest, selected->key, query.token_count);
    }
  }
  return Status{StatusCode::kNotFound, "cache prefix does not exist"};
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "unable to allocate prefix query"};
}

std::size_t MatchIndex::Size() const {
  std::shared_lock lock(mutex_);
  return size_;
}

}  // namespace kvstore::kvcache
