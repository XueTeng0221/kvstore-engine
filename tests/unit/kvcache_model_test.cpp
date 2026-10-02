#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

#include "kvstore/common/crc32.hpp"
#include "kvstore/kvcache/chunk_registry.hpp"
#include "kvstore/kvcache/model.hpp"

namespace kvstore::kvcache {
namespace {

TensorManifest Manifest() {
  TensorManifest manifest;
  manifest.tenant_id = "tenant-a";
  manifest.model_id = "Qwen/Qwen2.5-0.5B-Instruct";
  manifest.model_revision = "immutable-model-revision";
  manifest.tokenizer_revision = "immutable-tokenizer-revision";
  manifest.cache_format = "vllm-kvconnector-v1";
  manifest.cache_format_version = 1;
  const std::array<std::uint32_t, 4> token_ids{1, 2, 3, 4};
  manifest.token_digest = TokenDigest(token_ids).value();
  manifest.token_count = 4;
  manifest.layer_begin = 0;
  manifest.layer_count = 1;
  manifest.dtype = DType::kFloat16;
  manifest.layout = TensorLayout::kLayerMajor;
  manifest.device = {DeviceKind::kCuda, 0};
  manifest.shape = {2, 1, 4, 2, 4};
  manifest.axis_order = {TensorAxis::kKeyValue, TensorAxis::kLayer, TensorAxis::kToken,
                         TensorAxis::kHead, TensorAxis::kHeadDimension};
  manifest.strides_bytes = {64, 64, 16, 8, 2};
  manifest.payload_bytes = 128;
  manifest.chunk_bytes = 64;
  manifest.chunk_count = 2;
  Bytes expanded_payload(128, std::byte{'x'});
  manifest.payload_digest = Sha256(expanded_payload).value();
  manifest.created_at_ns = 10;
  manifest.accessed_at_ns = 20;
  return manifest;
}

TensorManifest BlockManifest(KeyValuePacking packing) {
  TensorManifest manifest = Manifest();
  manifest.layout = TensorLayout::kBlockMajor;
  manifest.key_value_packing = packing;
  manifest.block_tokens = 4;
  manifest.token_count = 8;
  if (packing == KeyValuePacking::kPlanar) {
    manifest.shape = {1, 2, 2, 4, 2, 2};
    manifest.axis_order = {TensorAxis::kLayer, TensorAxis::kKeyValue, TensorAxis::kBlock,
                           TensorAxis::kToken, TensorAxis::kHead,     TensorAxis::kHeadDimension};
    manifest.strides_bytes = {128, 64, 32, 8, 4, 2};
  } else {
    manifest.shape = {1, 2, 4, 2, 2, 2};
    manifest.axis_order = {TensorAxis::kLayer,    TensorAxis::kBlock, TensorAxis::kToken,
                           TensorAxis::kKeyValue, TensorAxis::kHead,  TensorAxis::kHeadDimension};
    manifest.strides_bytes = {128, 64, 16, 8, 4, 2};
  }
  return manifest;
}

Status PutPayload(ChunkRegistry& registry, std::uint64_t reservation, const Bytes& payload) {
  const ByteView first(payload.data(), 64);
  const ByteView second(payload.data() + 64, 64);
  Status status = registry.Put(reservation, 0, first, Crc32(first));
  if (!status.ok()) {
    return status;
  }
  return registry.Put(reservation, 1, second, Crc32(second));
}

TEST(KvCacheModelTest, Sha256UsesPublishedVector) {
  const std::array<std::byte, 3> input{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  const auto digest = Sha256(input);
  ASSERT_TRUE(digest.ok());
  EXPECT_EQ(DigestHex(digest.value()),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(KvCacheModelTest, TokenDigestUsesBigEndianUint32Encoding) {
  const std::array<std::uint32_t, 1> token_ids{0x01020304U};
  const std::array<std::byte, 4> encoded{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  ASSERT_TRUE(TokenDigest(token_ids).ok());
  ASSERT_TRUE(Sha256(encoded).ok());
  EXPECT_EQ(TokenDigest(token_ids).value(), Sha256(encoded).value());
}

TEST(KvCacheModelTest, CanonicalKeyIsDeterministicAndExcludesTimestamps) {
  TensorManifest first = Manifest();
  TensorManifest second = first;
  second.created_at_ns = 999;
  second.accessed_at_ns = 1000;

  const auto first_key = CanonicalCacheKey(first);
  const auto second_key = CanonicalCacheKey(second);
  ASSERT_TRUE(first_key.ok());
  ASSERT_TRUE(second_key.ok());
  EXPECT_EQ(first_key.value(), second_key.value());
  EXPECT_EQ(first_key.value().ToString(),
            "kvc1:42289701a2b511cf95219d0fc7d1501101a7f49b8f1e3a2fe14b6146f71e9372");

  second.tenant_id = "tenant-b";
  const auto isolated_key = CanonicalCacheKey(second);
  ASSERT_TRUE(isolated_key.ok());
  EXPECT_NE(first_key.value(), isolated_key.value());
}

TEST(KvCacheModelTest, CanonicalManifestRoundTripsForDiskIndexRebuild) {
  const TensorManifest expected = Manifest();
  const auto encoded = EncodeCanonicalManifest(expected);
  ASSERT_TRUE(encoded.ok());
  const auto decoded = DecodeCanonicalManifest(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.status().message();
  const auto reencoded = EncodeCanonicalManifest(decoded.value());
  ASSERT_TRUE(reencoded.ok());
  EXPECT_EQ(reencoded.value(), encoded.value());
  EXPECT_EQ(decoded.value().tenant_id, expected.tenant_id);
  EXPECT_EQ(decoded.value().token_count, expected.token_count);
  EXPECT_EQ(decoded.value().shape, expected.shape);
}

TEST(KvCacheModelTest, IdentityChangesInvalidateKeys) {
  const auto base = CanonicalCacheKey(Manifest());
  ASSERT_TRUE(base.ok());

  TensorManifest changed = Manifest();
  changed.model_revision = "new-model-revision";
  ASSERT_TRUE(CanonicalCacheKey(changed).ok());
  EXPECT_NE(base.value(), CanonicalCacheKey(changed).value());

  changed = Manifest();
  changed.adapter_id = "adapter";
  changed.adapter_revision = "adapter-v2";
  EXPECT_NE(base.value(), CanonicalCacheKey(changed).value());

  changed = Manifest();
  changed.tokenizer_revision = "tokenizer-v2";
  EXPECT_NE(base.value(), CanonicalCacheKey(changed).value());

  changed = Manifest();
  changed.token_digest[0] ^= std::byte{1};
  EXPECT_NE(base.value(), CanonicalCacheKey(changed).value());
}

TEST(KvCacheModelTest, RejectsIncompatibleAndOverflowingMetadata) {
  TensorManifest manifest = Manifest();
  manifest.adapter_id = "adapter-without-revision";
  EXPECT_EQ(ValidateManifest(manifest).code(), StatusCode::kInvalidArgument);

  manifest = Manifest();
  manifest.payload_bytes = 7;
  EXPECT_EQ(ValidateManifest(manifest).code(), StatusCode::kInvalidArgument);

  manifest = Manifest();
  manifest.shape = {2, 1, 4, UINT64_MAX, 2};
  EXPECT_EQ(ValidateManifest(manifest).code(), StatusCode::kLimitExceeded);

  manifest = Manifest();
  manifest.tensor_parallel_rank = 1;
  EXPECT_EQ(ValidateManifest(manifest).code(), StatusCode::kInvalidArgument);

  manifest = Manifest();
  manifest.chunk_alignment_bytes = 32;
  EXPECT_EQ(ValidateManifest(manifest).code(), StatusCode::kInvalidArgument);

  manifest = Manifest();
  manifest.compression = static_cast<Compression>(99);
  EXPECT_EQ(ValidateManifest(manifest).code(), StatusCode::kUnsupported);

  manifest = Manifest();
  manifest.key_value_packing = KeyValuePacking::kInterleaved;
  EXPECT_EQ(ValidateManifest(manifest).code(), StatusCode::kInvalidArgument);

  manifest = Manifest();
  std::swap(manifest.axis_order[0], manifest.axis_order[1]);
  EXPECT_EQ(ValidateManifest(manifest).code(), StatusCode::kInvalidArgument);
}

TEST(KvCacheModelTest, AcceptsFullFinalBlockForBothBlockMajorPackings) {
  const TensorManifest planar = BlockManifest(KeyValuePacking::kPlanar);
  const TensorManifest interleaved = BlockManifest(KeyValuePacking::kInterleaved);
  EXPECT_TRUE(ValidateManifest(planar).ok());
  EXPECT_TRUE(ValidateManifest(interleaved).ok());
  ASSERT_TRUE(CanonicalCacheKey(planar).ok());
  ASSERT_TRUE(CanonicalCacheKey(interleaved).ok());
  EXPECT_NE(CanonicalCacheKey(planar).value(), CanonicalCacheKey(interleaved).value());

  TensorManifest extra_empty_block = planar;
  extra_empty_block.token_count = 4;
  EXPECT_EQ(ValidateManifest(extra_empty_block).code(), StatusCode::kInvalidArgument);
}

TEST(KvCacheModelTest, CoalescesPublicationByRejectingDuplicateReservation) {
  ChunkRegistry registry;
  const auto first = registry.Reserve(Manifest());
  ASSERT_TRUE(first.ok());
  const auto duplicate = registry.Reserve(Manifest());
  EXPECT_EQ(duplicate.status().code(), StatusCode::kBusy);

  TensorManifest conflicting = Manifest();
  conflicting.payload_digest[0] ^= std::byte{1};
  const auto conflict = registry.Reserve(conflicting);
  EXPECT_EQ(conflict.status().code(), StatusCode::kCorruption);
  EXPECT_TRUE(registry.Abort(first.value()).ok());
}

TEST(KvCacheModelTest, IncompleteObjectIsInvisibleAndCommitIsAtomic) {
  ChunkRegistry registry;
  TensorManifest manifest = Manifest();
  const auto reservation = registry.Reserve(manifest);
  ASSERT_TRUE(reservation.ok());
  EXPECT_EQ(registry.Pending(), 1U);
  EXPECT_EQ(registry.Lookup(manifest).status().code(), StatusCode::kNotFound);

  const Bytes payload(128, std::byte{'x'});
  const ByteView first(payload.data(), 64);
  ASSERT_TRUE(registry.Put(reservation.value(), 0, first, Crc32(first)).ok());
  EXPECT_EQ(registry.Commit(reservation.value()).status().code(), StatusCode::kBusy);
  const ByteView second(payload.data() + 64, 64);
  ASSERT_TRUE(registry.Put(reservation.value(), 1, second, Crc32(second)).ok());

  auto committed = registry.Commit(reservation.value());
  ASSERT_TRUE(committed.ok());
  EXPECT_EQ(registry.Pending(), 0U);
  EXPECT_EQ(registry.Size(), 1U);
  ASSERT_TRUE(committed.value().Chunk(0).ok());
  ASSERT_TRUE(committed.value().Chunk(1).ok());
  EXPECT_TRUE(std::ranges::equal(committed.value().Chunk(0).value(), first));
  EXPECT_TRUE(std::ranges::equal(committed.value().Chunk(1).value(), second));
  EXPECT_EQ(committed.value().Chunk(2).status().code(), StatusCode::kInvalidArgument);
}

TEST(KvCacheModelTest, DetectsChunkAndWholePayloadCorruption) {
  ChunkRegistry registry;
  const Bytes payload(128, std::byte{'x'});
  auto reservation = registry.Reserve(Manifest());
  ASSERT_TRUE(reservation.ok());
  const ByteView first(payload.data(), 64);
  EXPECT_EQ(registry.Put(reservation.value(), 0, first, Crc32(first) + 1U).code(),
            StatusCode::kCorruption);
  ASSERT_TRUE(registry.Abort(reservation.value()).ok());

  TensorManifest manifest = Manifest();
  Bytes corrupted = payload;
  corrupted[127] ^= std::byte{1};
  reservation = registry.Reserve(manifest);
  ASSERT_TRUE(reservation.ok());
  ASSERT_TRUE(PutPayload(registry, reservation.value(), corrupted).ok());
  EXPECT_EQ(registry.Commit(reservation.value()).status().code(), StatusCode::kCorruption);
  ASSERT_TRUE(registry.Abort(reservation.value()).ok());
}

TEST(KvCacheModelTest, DeleteHidesObjectButExistingHandleRetainsChunks) {
  ChunkRegistry registry;
  const Bytes payload(128, std::byte{'x'});
  const auto reservation = registry.Reserve(Manifest());
  ASSERT_TRUE(reservation.ok());
  ASSERT_TRUE(PutPayload(registry, reservation.value(), payload).ok());
  auto handle = registry.Commit(reservation.value());
  ASSERT_TRUE(handle.ok());

  TensorManifest expected = Manifest();
  expected.payload_digest = {};
  ASSERT_TRUE(registry.Delete(expected).ok());
  EXPECT_EQ(registry.Lookup(expected).status().code(), StatusCode::kNotFound);
  ASSERT_TRUE(handle.value().Chunk(0).ok());
  EXPECT_TRUE(std::ranges::equal(handle.value().Chunk(0).value(), ByteView(payload.data(), 64)));
}

TEST(KvCacheModelTest, DetectsInjectedCanonicalKeyCollision) {
  const auto constant_key = [](const TensorManifest&) -> Result<CacheKey> {
    return CacheKey{kManifestVersion, {}};
  };
  ChunkRegistry registry({}, constant_key);
  const Bytes payload(128, std::byte{'x'});
  auto reservation = registry.Reserve(Manifest());
  ASSERT_TRUE(reservation.ok());
  ASSERT_TRUE(PutPayload(registry, reservation.value(), payload).ok());
  ASSERT_TRUE(registry.Commit(reservation.value()).ok());

  TensorManifest other = Manifest();
  other.tenant_id = "other-tenant";
  EXPECT_EQ(registry.Lookup(other).status().code(), StatusCode::kCorruption);
  const auto collided = registry.Reserve(other);
  EXPECT_EQ(collided.status().code(), StatusCode::kCorruption);
}

TEST(KvCacheModelTest, MapsInjectedKeyFunctionFailureToStatus) {
  ChunkRegistry registry({}, [](const TensorManifest&) -> Result<CacheKey> { throw 7; });
  EXPECT_EQ(registry.Reserve(Manifest()).status().code(), StatusCode::kInternal);
}

TEST(KvCacheModelTest, ConcurrentChunkUploadsPublishOneCompleteObject) {
  ChunkRegistry registry;
  const Bytes payload(128, std::byte{'x'});
  const auto reservation = registry.Reserve(Manifest());
  ASSERT_TRUE(reservation.ok());
  Status first_status;
  Status second_status;
  std::thread first([&] {
    const ByteView chunk(payload.data(), 64);
    first_status = registry.Put(reservation.value(), 0, chunk, Crc32(chunk));
  });
  std::thread second([&] {
    const ByteView chunk(payload.data() + 64, 64);
    second_status = registry.Put(reservation.value(), 1, chunk, Crc32(chunk));
  });
  first.join();
  second.join();
  EXPECT_TRUE(first_status.ok());
  EXPECT_TRUE(second_status.ok());
  EXPECT_TRUE(registry.Commit(reservation.value()).ok());
}

TEST(KvCacheModelTest, EnforcesReservationAndTrackedByteLimits) {
  RegistryLimits limits;
  limits.max_objects = 1;
  limits.max_pending_reservations = 1;
  limits.max_chunks_per_object = 2;
  limits.max_object_bytes = 128;
  limits.max_pending_bytes = 2048;
  limits.max_tracked_bytes = 4000;
  ChunkRegistry registry(limits);
  const Bytes payload(128, std::byte{'x'});
  auto reservation = registry.Reserve(Manifest());
  ASSERT_TRUE(reservation.ok());
  const std::uint64_t accounted_bytes = registry.TrackedBytes();
  EXPECT_GT(accounted_bytes, 128U);
  EXPECT_LE(accounted_bytes, limits.max_tracked_bytes);

  TensorManifest other = Manifest();
  other.tenant_id = "other";
  EXPECT_EQ(registry.Reserve(other).status().code(), StatusCode::kLimitExceeded);
  ASSERT_TRUE(PutPayload(registry, reservation.value(), payload).ok());
  auto handle = registry.Commit(reservation.value());
  ASSERT_TRUE(handle.ok());
  EXPECT_EQ(registry.TrackedBytes(), accounted_bytes);
  ASSERT_TRUE(registry.Delete(Manifest()).ok());
  EXPECT_EQ(registry.TrackedBytes(), accounted_bytes);
  EXPECT_EQ(registry.Reserve(other).status().code(), StatusCode::kLimitExceeded);

  handle = Status{StatusCode::kNotFound, "release test handle"};
  EXPECT_EQ(registry.TrackedBytes(), 0U);
  EXPECT_TRUE(registry.Reserve(other).ok());
}

TEST(KvCacheModelTest, ConcurrentSameKeyReserveHasSingleWinner) {
  ChunkRegistry registry;
  Result<std::uint64_t> first(Status{StatusCode::kInternal, "not run"});
  Result<std::uint64_t> second(Status{StatusCode::kInternal, "not run"});
  std::thread first_thread([&] { first = registry.Reserve(Manifest()); });
  std::thread second_thread([&] { second = registry.Reserve(Manifest()); });
  first_thread.join();
  second_thread.join();
  EXPECT_NE(first.ok(), second.ok());
  EXPECT_EQ(first.ok() ? second.status().code() : first.status().code(), StatusCode::kBusy);
  ASSERT_TRUE(registry.Abort(first.ok() ? first.value() : second.value()).ok());
}

TEST(KvCacheModelTest, PutRacesAbortWithoutUseAfterFree) {
  ChunkRegistry registry;
  const auto reservation = registry.Reserve(Manifest());
  ASSERT_TRUE(reservation.ok());
  const Bytes payload(64, std::byte{'x'});
  Status put_status;
  Status abort_status;
  std::thread put([&] {
    const ByteView chunk(payload);
    put_status = registry.Put(reservation.value(), 0, chunk, Crc32(chunk));
  });
  std::thread abort([&] { abort_status = registry.Abort(reservation.value()); });
  put.join();
  abort.join();
  EXPECT_TRUE(abort_status.ok() || abort_status.code() == StatusCode::kNotFound);
  EXPECT_TRUE(put_status.ok() || put_status.code() == StatusCode::kNotFound);
  EXPECT_EQ(registry.Pending(), 0U);
}

TEST(KvCacheModelTest, CommitAndAbortRaceHasOneTerminalWinner) {
  ChunkRegistry registry;
  const Bytes payload(128, std::byte{'x'});
  const auto reservation = registry.Reserve(Manifest());
  ASSERT_TRUE(reservation.ok());
  ASSERT_TRUE(PutPayload(registry, reservation.value(), payload).ok());
  Result<ResidentHandle> commit(Status{StatusCode::kInternal, "not run"});
  Status abort_status;
  std::thread commit_thread([&] { commit = registry.Commit(reservation.value()); });
  std::thread abort_thread([&] { abort_status = registry.Abort(reservation.value()); });
  commit_thread.join();
  abort_thread.join();
  EXPECT_NE(commit.ok(), abort_status.ok());
  EXPECT_EQ(commit.ok() ? abort_status.code() : commit.status().code(), StatusCode::kNotFound);
  EXPECT_EQ(registry.Pending(), 0U);
}

TEST(KvCacheModelTest, DistinctPendingCommitsCannotBypassObjectLimit) {
  RegistryLimits limits;
  limits.max_objects = 1;
  limits.max_pending_reservations = 2;
  limits.max_pending_bytes = 4096;
  limits.max_tracked_bytes = 4096;
  ChunkRegistry registry(limits);
  const Bytes payload(128, std::byte{'x'});
  TensorManifest first_manifest = Manifest();
  TensorManifest second_manifest = Manifest();
  second_manifest.tenant_id = "tenant-b";
  const auto first = registry.Reserve(first_manifest);
  const auto second = registry.Reserve(second_manifest);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  ASSERT_TRUE(PutPayload(registry, first.value(), payload).ok());
  ASSERT_TRUE(PutPayload(registry, second.value(), payload).ok());
  EXPECT_TRUE(registry.Commit(first.value()).ok());
  EXPECT_EQ(registry.Commit(second.value()).status().code(), StatusCode::kLimitExceeded);
  EXPECT_EQ(registry.Size(), 1U);
  EXPECT_EQ(registry.Pending(), 1U);
  EXPECT_TRUE(registry.Abort(second.value()).ok());
}

TEST(KvCacheModelTest, LookupRacesDeleteWithStableHandleLifetime) {
  ChunkRegistry registry;
  const Bytes payload(128, std::byte{'x'});
  const auto reservation = registry.Reserve(Manifest());
  ASSERT_TRUE(reservation.ok());
  ASSERT_TRUE(PutPayload(registry, reservation.value(), payload).ok());
  ASSERT_TRUE(registry.Commit(reservation.value()).ok());
  Result<ResidentHandle> lookup(Status{StatusCode::kInternal, "not run"});
  Status delete_status;
  std::thread lookup_thread([&] { lookup = registry.Lookup(Manifest()); });
  std::thread delete_thread([&] { delete_status = registry.Delete(Manifest()); });
  lookup_thread.join();
  delete_thread.join();
  EXPECT_TRUE(delete_status.ok());
  EXPECT_TRUE(lookup.ok() || lookup.status().code() == StatusCode::kNotFound);
  if (lookup.ok()) {
    EXPECT_TRUE(lookup.value().Chunk(0).ok());
  }
}

}  // namespace
}  // namespace kvstore::kvcache
