#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "kvstore/kvcache/tiered_store.hpp"

namespace kvstore::kvcache {
namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::array<char, 32> name{};
    const std::string pattern = "/tmp/kvstore-tiered-XXXXXX";
    std::copy(pattern.begin(), pattern.end(), name.begin());
    path_ = ::mkdtemp(name.data());
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

struct ObjectFixture {
  TensorManifest manifest;
  Bytes payload;
  std::vector<ByteView> chunks;

  ObjectFixture() : payload(128, std::byte{'x'}) {
    manifest.tenant_id = "tenant-a";
    manifest.model_id = "model";
    manifest.model_revision = "model-v1";
    manifest.tokenizer_revision = "tokenizer-v1";
    manifest.cache_format = "test-v1";
    const std::array<std::uint32_t, 4> tokens{1, 2, 3, 4};
    manifest.token_digest = TokenDigest(tokens).value();
    manifest.token_count = tokens.size();
    manifest.layer_count = 1;
    manifest.shape = {2, 1, 4, 2, 4};
    manifest.axis_order = {TensorAxis::kKeyValue, TensorAxis::kLayer, TensorAxis::kToken,
                           TensorAxis::kHead, TensorAxis::kHeadDimension};
    manifest.strides_bytes = {64, 64, 16, 8, 2};
    manifest.payload_bytes = payload.size();
    manifest.chunk_bytes = 64;
    manifest.chunk_count = 2;
    manifest.payload_digest = Sha256(payload).value();
    chunks.emplace_back(payload.data(), 64);
    chunks.emplace_back(payload.data() + 64, 64);
  }
};

TieredStoreConfig Config(const std::filesystem::path& directory) {
  return {directory, {1024, 768, 256}, 4096, 1024, 8, 1024, {}};
}

std::filesystem::path ObjectDirectory(const std::filesystem::path& root,
                                      const TensorManifest& manifest) {
  const auto key = CanonicalCacheKey(manifest).value();
  const std::string hex = DigestHex(key.digest);
  return root / "objects" / hex.substr(0, 2) / hex;
}

std::filesystem::path FirstChunkPath(const std::filesystem::path& root,
                                     const TensorManifest& manifest) {
  const auto directory = ObjectDirectory(root, manifest);
  for (const auto& item : std::filesystem::directory_iterator(directory)) {
    if (item.path().filename() != "object.meta") return item.path();
  }
  return {};
}

TEST(KvCacheTierStateTest, AcceptsOnlyDefinedTransitions) {
  EXPECT_TRUE(ValidateTierTransition(TierState::kResident, TierState::kEvicting).ok());
  EXPECT_TRUE(ValidateTierTransition(TierState::kEvicting, TierState::kDiskOnly).ok());
  EXPECT_TRUE(ValidateTierTransition(TierState::kDiskOnly, TierState::kLoading).ok());
  EXPECT_TRUE(ValidateTierTransition(TierState::kLoading, TierState::kResident).ok());
  EXPECT_TRUE(ValidateTierTransition(TierState::kFailed, TierState::kLoading).ok());
  EXPECT_EQ(ValidateTierTransition(TierState::kResident, TierState::kLoading).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(ValidateTierTransition(TierState::kDiskOnly, TierState::kResident).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(ValidateTierTransition(TierState::kFailed, TierState::kResident).code(),
            StatusCode::kInvalidArgument);
}

TEST(KvCacheResidentPoolTest, EnforcesAlignmentBudgetWatermarksAndStats) {
  ResidentPool pool({128, 128, 64});
  const Bytes first(65, std::byte{1});
  auto allocation = pool.Copy(first);
  ASSERT_TRUE(allocation.ok());
  EXPECT_EQ(allocation.value().address() % kChunkAlignmentBytes, 0U);
  auto stats = pool.Stats();
  EXPECT_EQ(stats.used_bytes, 128U);
  EXPECT_EQ(stats.requested_bytes, 65U);
  EXPECT_EQ(stats.fragmentation_bytes, 63U);
  EXPECT_TRUE(stats.above_high_watermark);
  EXPECT_FALSE(stats.below_low_watermark);
  const Bytes extra(1, std::byte{2});
  EXPECT_EQ(pool.Copy(extra).status().code(), StatusCode::kLimitExceeded);
  const auto released_address = allocation.value().address();
  allocation = Status{StatusCode::kNotFound, "release"};
  stats = pool.Stats();
  EXPECT_EQ(stats.used_bytes, 128U);
  EXPECT_FALSE(stats.below_low_watermark);
  EXPECT_EQ(stats.peak_used_bytes, 128U);
  auto reused = pool.Copy(first);
  ASSERT_TRUE(reused.ok());
  EXPECT_EQ(reused.value().address(), released_address);
}

TEST(KvCacheTieredStoreTest, PersistsEvictsPromotesAndReopens) {
  TemporaryDirectory temporary;
  ObjectFixture object;
  auto opened = TieredStore::Open(Config(temporary.path()));
  ASSERT_TRUE(opened.ok());
  auto store = std::move(opened.value());
  auto handle = store->Put(object.manifest, object.chunks);
  ASSERT_TRUE(handle.ok());
  EXPECT_EQ(store->State(object.manifest).value(), TierState::kResident);
  ASSERT_TRUE(store->Evict(object.manifest).ok());
  EXPECT_EQ(store->State(object.manifest).value(), TierState::kDiskOnly);
  auto promoted = store->Lookup(object.manifest);
  ASSERT_TRUE(promoted.ok());
  EXPECT_EQ(store->Stats().disk_read_count, 1U);
  EXPECT_TRUE(std::ranges::equal(promoted.value().Chunk(0).value(), object.chunks[0]));
  store.reset();

  opened = TieredStore::Open(Config(temporary.path()));
  ASSERT_TRUE(opened.ok());
  EXPECT_EQ(opened.value()->State(object.manifest).value(), TierState::kDiskOnly);
  EXPECT_TRUE(opened.value()->Lookup(object.manifest).ok());
}

TEST(KvCacheTieredStoreTest, EnforcesDiskQuotaAndDeleteReclaimsIt) {
  TemporaryDirectory temporary;
  ObjectFixture object;
  auto config = Config(temporary.path());
  config.disk_budget_bytes = 100;
  auto opened = TieredStore::Open(config);
  ASSERT_TRUE(opened.ok());
  EXPECT_EQ(opened.value()->Put(object.manifest, object.chunks).status().code(),
            StatusCode::kLimitExceeded);

  config.disk_budget_bytes = 4096;
  opened = TieredStore::Open(config);
  ASSERT_TRUE(opened.ok());
  ASSERT_TRUE(opened.value()->Put(object.manifest, object.chunks).ok());
  EXPECT_GT(opened.value()->Stats().disk_used_bytes, 0U);
  EXPECT_TRUE(opened.value()->Delete(object.manifest).ok());
  EXPECT_EQ(opened.value()->Stats().disk_used_bytes, 0U);
  EXPECT_FALSE(std::filesystem::exists(ObjectDirectory(temporary.path(), object.manifest)));
}

TEST(KvCacheTieredStoreTest, ConcurrentPublicationsCannotOvercommitDiskQuota) {
  TemporaryDirectory temporary;
  ObjectFixture first;
  ObjectFixture second;
  second.manifest.tenant_id = "tenant-b";
  auto config = Config(temporary.path());
  config.disk_budget_bytes = 650;
  auto opened = TieredStore::Open(config);
  ASSERT_TRUE(opened.ok());
  std::barrier start(2);
  std::array<StatusCode, 2> results{};
  std::jthread first_writer([&] {
    start.arrive_and_wait();
    const auto result = opened.value()->Put(first.manifest, first.chunks);
    results[0] = result.ok() ? StatusCode::kOk : result.status().code();
  });
  std::jthread second_writer([&] {
    start.arrive_and_wait();
    const auto result = opened.value()->Put(second.manifest, second.chunks);
    results[1] = result.ok() ? StatusCode::kOk : result.status().code();
  });
  first_writer.join();
  second_writer.join();
  EXPECT_NE(results[0], results[1]);
  EXPECT_TRUE(results[0] == StatusCode::kOk || results[1] == StatusCode::kOk);
  EXPECT_TRUE(results[0] == StatusCode::kLimitExceeded || results[1] == StatusCode::kLimitExceeded);
  EXPECT_LE(opened.value()->Stats().disk_used_bytes, config.disk_budget_bytes);
}

TEST(KvCacheTieredStoreTest, InjectedEnospcRollsBackPublicationAndQuota) {
  TemporaryDirectory temporary;
  ObjectFixture object;
  auto config = Config(temporary.path());
  config.io_fault = [](std::string_view stage) {
    return stage == "chunk.write" ? Status{StatusCode::kIoError, "injected ENOSPC"} : Status::Ok();
  };
  auto opened = TieredStore::Open(config);
  ASSERT_TRUE(opened.ok());
  EXPECT_EQ(opened.value()->Put(object.manifest, object.chunks).status().code(),
            StatusCode::kIoError);
  EXPECT_EQ(opened.value()->Stats().disk_used_bytes, 0U);
  EXPECT_EQ(opened.value()->Stats().object_count, 0U);
  EXPECT_EQ(opened.value()->State(object.manifest).status().code(), StatusCode::kNotFound);
  EXPECT_FALSE(std::filesystem::exists(ObjectDirectory(temporary.path(), object.manifest)));

  config.io_fault = {};
  opened = TieredStore::Open(config);
  ASSERT_TRUE(opened.ok());
  EXPECT_TRUE(opened.value()->Put(object.manifest, object.chunks).ok());
}

TEST(KvCacheTieredStoreTest, PartialWriteFailureRollsBackTemporaryObject) {
  TemporaryDirectory temporary;
  ObjectFixture object;
  auto config = Config(temporary.path());
  config.io_fault = [](std::string_view stage) {
    return stage == "chunk.write.after-progress"
               ? Status{StatusCode::kIoError, "injected ENOSPC after partial write"}
               : Status::Ok();
  };
  auto opened = TieredStore::Open(config);
  ASSERT_TRUE(opened.ok());
  EXPECT_EQ(opened.value()->Put(object.manifest, object.chunks).status().code(),
            StatusCode::kIoError);
  EXPECT_EQ(opened.value()->Stats().disk_used_bytes, 0U);
  EXPECT_EQ(opened.value()->Stats().object_count, 0U);
  EXPECT_FALSE(std::filesystem::exists(ObjectDirectory(temporary.path(), object.manifest)));
}

TEST(KvCacheTieredStoreTest, MetadataPublishFailureLeavesNoVisibleObject) {
  TemporaryDirectory temporary;
  ObjectFixture object;
  auto config = Config(temporary.path());
  config.io_fault = [](std::string_view stage) {
    return stage == "metadata.rename" ? Status{StatusCode::kIoError, "injected rename failure"}
                                      : Status::Ok();
  };
  auto opened = TieredStore::Open(config);
  ASSERT_TRUE(opened.ok());
  EXPECT_EQ(opened.value()->Put(object.manifest, object.chunks).status().code(),
            StatusCode::kIoError);
  EXPECT_EQ(opened.value()->Stats().object_count, 0U);
  EXPECT_FALSE(std::filesystem::exists(ObjectDirectory(temporary.path(), object.manifest)));
}

TEST(KvCacheTieredStoreTest, PostMetadataSyncFailureRemainsChargedAndRecoverable) {
  TemporaryDirectory temporary;
  ObjectFixture object;
  auto config = Config(temporary.path());
  config.io_fault = [](std::string_view stage) {
    return stage == "metadata.dirsync"
               ? Status{StatusCode::kIoError, "injected metadata directory sync failure"}
               : Status::Ok();
  };
  auto opened = TieredStore::Open(config);
  ASSERT_TRUE(opened.ok());
  EXPECT_EQ(opened.value()->Put(object.manifest, object.chunks).status().code(),
            StatusCode::kIoError);
  EXPECT_EQ(opened.value()->State(object.manifest).value(), TierState::kFailed);
  EXPECT_GT(opened.value()->Stats().disk_used_bytes, 0U);
  opened = Status{StatusCode::kNotFound, "close"};

  config.io_fault = {};
  opened = TieredStore::Open(config);
  ASSERT_TRUE(opened.ok());
  EXPECT_EQ(opened.value()->State(object.manifest).value(), TierState::kDiskOnly);
  EXPECT_TRUE(opened.value()->Lookup(object.manifest).ok());
}

TEST(KvCacheTieredStoreTest, DeleteSyncFailureDoesNotStrandEvictingState) {
  TemporaryDirectory temporary;
  ObjectFixture object;
  auto fail_delete_sync = std::make_shared<std::atomic<bool>>(false);
  auto config = Config(temporary.path());
  config.io_fault = [fail_delete_sync](std::string_view stage) {
    return fail_delete_sync->load() && stage == "delete.shard.fsync"
               ? Status{StatusCode::kIoError, "injected delete sync failure"}
               : Status::Ok();
  };
  auto opened = TieredStore::Open(config);
  ASSERT_TRUE(opened.ok());
  ASSERT_TRUE(opened.value()->Put(object.manifest, object.chunks).ok());
  fail_delete_sync->store(true);
  EXPECT_EQ(opened.value()->Delete(object.manifest).code(), StatusCode::kIoError);
  EXPECT_EQ(opened.value()->State(object.manifest).status().code(), StatusCode::kNotFound);
  EXPECT_EQ(opened.value()->Stats().disk_used_bytes, 0U);
}

TEST(KvCacheTieredStoreTest, ThrowingDeleteHookDoesNotStrandEvictingState) {
  TemporaryDirectory temporary;
  ObjectFixture object;
  auto throw_on_delete = std::make_shared<std::atomic<bool>>(false);
  auto config = Config(temporary.path());
  config.io_fault = [throw_on_delete](std::string_view stage) -> Status {
    if (throw_on_delete->load() && stage == "delete.shard.fsync") throw 7;
    return Status::Ok();
  };
  auto opened = TieredStore::Open(config);
  ASSERT_TRUE(opened.ok());
  ASSERT_TRUE(opened.value()->Put(object.manifest, object.chunks).ok());
  throw_on_delete->store(true);
  EXPECT_EQ(opened.value()->Delete(object.manifest).code(), StatusCode::kInternal);
  EXPECT_EQ(opened.value()->State(object.manifest).status().code(), StatusCode::kNotFound);
  EXPECT_EQ(opened.value()->Stats().disk_used_bytes, 0U);
}

TEST(KvCacheTieredStoreTest, DetectsTruncatedAndCorruptChunks) {
  TemporaryDirectory temporary;
  ObjectFixture object;
  auto opened = TieredStore::Open(Config(temporary.path()));
  ASSERT_TRUE(opened.ok());
  ASSERT_TRUE(opened.value()->Put(object.manifest, object.chunks).ok());
  ASSERT_TRUE(opened.value()->Evict(object.manifest).ok());
  const auto chunk = FirstChunkPath(temporary.path(), object.manifest);
  ASSERT_FALSE(chunk.empty());
  ASSERT_EQ(::truncate(chunk.c_str(), 3), 0);
  EXPECT_EQ(opened.value()->Lookup(object.manifest).status().code(), StatusCode::kCorruption);

  TemporaryDirectory corrupt_temporary;
  opened = TieredStore::Open(Config(corrupt_temporary.path()));
  ASSERT_TRUE(opened.ok());
  ASSERT_TRUE(opened.value()->Put(object.manifest, object.chunks).ok());
  ASSERT_TRUE(opened.value()->Evict(object.manifest).ok());
  const auto corrupt_chunk = FirstChunkPath(corrupt_temporary.path(), object.manifest);
  const int fd = ::open(corrupt_chunk.c_str(), O_WRONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  const std::byte changed{'z'};
  ASSERT_EQ(::pwrite(fd, &changed, 1, 0), 1);
  ASSERT_EQ(::close(fd), 0);
  EXPECT_EQ(opened.value()->Lookup(object.manifest).status().code(), StatusCode::kCorruption);
}

TEST(KvCacheTieredStoreTest, PartialReadFailureLeavesRetryableDiskObject) {
  TemporaryDirectory temporary;
  ObjectFixture object;
  auto fail_read = std::make_shared<std::atomic<bool>>(false);
  auto config = Config(temporary.path());
  config.io_fault = [fail_read](std::string_view stage) {
    return fail_read->load() && stage == "chunk.read.after-progress"
               ? Status{StatusCode::kIoError, "injected EIO after partial read"}
               : Status::Ok();
  };
  auto opened = TieredStore::Open(config);
  ASSERT_TRUE(opened.ok());
  auto initial = opened.value()->Put(object.manifest, object.chunks);
  ASSERT_TRUE(initial.ok());
  ASSERT_TRUE(opened.value()->Evict(object.manifest).ok());
  initial = Status{StatusCode::kNotFound, "release"};
  fail_read->store(true);
  EXPECT_EQ(opened.value()->Lookup(object.manifest).status().code(), StatusCode::kIoError);
  EXPECT_EQ(opened.value()->State(object.manifest).value(), TierState::kDiskOnly);
  fail_read->store(false);
  EXPECT_TRUE(opened.value()->Lookup(object.manifest).ok());
}

TEST(KvCacheTieredStoreTest, MetadataReadAfterProgressFaultIsInjectedOnRestart) {
  TemporaryDirectory temporary;
  ObjectFixture object;
  auto opened = TieredStore::Open(Config(temporary.path()));
  ASSERT_TRUE(opened.ok());
  ASSERT_TRUE(opened.value()->Put(object.manifest, object.chunks).ok());
  opened = Status{StatusCode::kNotFound, "close"};

  auto config = Config(temporary.path());
  config.io_fault = [](std::string_view stage) {
    return stage == "metadata.read.after-progress"
               ? Status{StatusCode::kIoError, "injected metadata read failure"}
               : Status::Ok();
  };
  EXPECT_EQ(TieredStore::Open(config).status().code(), StatusCode::kIoError);
}

TEST(KvCacheTieredStoreTest, DetectsMetadataCrcCorruptionOnRestart) {
  TemporaryDirectory temporary;
  ObjectFixture object;
  auto opened = TieredStore::Open(Config(temporary.path()));
  ASSERT_TRUE(opened.ok());
  ASSERT_TRUE(opened.value()->Put(object.manifest, object.chunks).ok());
  opened = Status{StatusCode::kNotFound, "close"};
  const auto metadata = ObjectDirectory(temporary.path(), object.manifest) / "object.meta";
  const int fd = ::open(metadata.c_str(), O_WRONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  const std::byte changed{1};
  ASSERT_EQ(::pwrite(fd, &changed, 1, 10), 1);
  ASSERT_EQ(::close(fd), 0);
  EXPECT_EQ(TieredStore::Open(Config(temporary.path())).status().code(), StatusCode::kCorruption);
}

TEST(KvCacheTieredStoreTest, RestartRejectsWrongShardAndSymlinkRoot) {
  TemporaryDirectory temporary;
  ObjectFixture object;
  auto opened = TieredStore::Open(Config(temporary.path()));
  ASSERT_TRUE(opened.ok());
  ASSERT_TRUE(opened.value()->Put(object.manifest, object.chunks).ok());
  opened = Status{StatusCode::kNotFound, "close"};
  const auto original = ObjectDirectory(temporary.path(), object.manifest);
  const auto wrong_shard = temporary.path() / "objects" / "ff" / original.filename();
  std::filesystem::create_directories(wrong_shard.parent_path());
  std::filesystem::rename(original, wrong_shard);
  EXPECT_EQ(TieredStore::Open(Config(temporary.path())).status().code(), StatusCode::kCorruption);

  TemporaryDirectory target;
  const auto link = temporary.path() / "linked-store";
  std::filesystem::create_directory_symlink(target.path(), link);
  EXPECT_EQ(TieredStore::Open(Config(link)).status().code(), StatusCode::kIoError);
}

TEST(KvCacheTieredStoreTest, RestartRejectsSymlinkShardWithoutTouchingTarget) {
  TemporaryDirectory temporary;
  TemporaryDirectory external;
  const auto external_object = external.path() / "outside-object";
  std::filesystem::create_directory(external_object);
  const auto shard = temporary.path() / "objects" / "aa";
  std::filesystem::create_directories(shard.parent_path());
  std::filesystem::create_directory_symlink(external.path(), shard);
  EXPECT_EQ(TieredStore::Open(Config(temporary.path())).status().code(), StatusCode::kCorruption);
  EXPECT_TRUE(std::filesystem::exists(external_object));
}

TEST(KvCacheTieredStoreTest, PromotionRestoresPersistedIntegrityMetadata) {
  TemporaryDirectory temporary;
  ObjectFixture object;
  object.manifest.created_at_ns = 111;
  object.manifest.accessed_at_ns = 222;
  auto opened = TieredStore::Open(Config(temporary.path()));
  ASSERT_TRUE(opened.ok());
  ASSERT_TRUE(opened.value()->Put(object.manifest, object.chunks).ok());
  opened = Status{StatusCode::kNotFound, "close"};

  opened = TieredStore::Open(Config(temporary.path()));
  ASSERT_TRUE(opened.ok());
  TensorManifest expected = object.manifest;
  expected.payload_digest = {};
  expected.created_at_ns = 999;
  expected.accessed_at_ns = 1000;
  const auto promoted = opened.value()->Lookup(expected);
  ASSERT_TRUE(promoted.ok());
  EXPECT_EQ(promoted.value().manifest().payload_digest, object.manifest.payload_digest);
  EXPECT_EQ(promoted.value().manifest().created_at_ns, 111U);
  EXPECT_EQ(promoted.value().manifest().accessed_at_ns, 222U);
}

TEST(KvCacheTieredStoreTest, CancellationAndDeadlineDoNotStartIo) {
  TemporaryDirectory temporary;
  ObjectFixture object;
  auto opened = TieredStore::Open(Config(temporary.path()));
  ASSERT_TRUE(opened.ok());
  ASSERT_TRUE(opened.value()->Put(object.manifest, object.chunks).ok());
  ASSERT_TRUE(opened.value()->Evict(object.manifest).ok());
  std::stop_source source;
  source.request_stop();
  EXPECT_EQ(opened.value()
                ->Lookup(object.manifest, TieredStore::Deadline::max(), source.get_token())
                .status()
                .code(),
            StatusCode::kCancelled);
  EXPECT_EQ(
      opened.value()->Lookup(object.manifest, std::chrono::steady_clock::now()).status().code(),
      StatusCode::kCancelled);
  EXPECT_EQ(opened.value()->Stats().disk_read_count, 0U);
}

TEST(KvCacheTieredStoreTest, CancelledWaiterDoesNotCancelSharedPromotion) {
  TemporaryDirectory temporary;
  ObjectFixture object;
  constexpr std::size_t kPayloadBytes = 16U * 1024U * 1024U;
  object.payload.assign(kPayloadBytes, std::byte{'p'});
  object.manifest.token_count = kPayloadBytes / 32U;
  object.manifest.shape[2] = object.manifest.token_count;
  object.manifest.strides_bytes = {kPayloadBytes / 2U, kPayloadBytes / 2U, 16, 8, 2};
  object.manifest.payload_bytes = kPayloadBytes;
  object.manifest.chunk_bytes = kPayloadBytes;
  object.manifest.chunk_count = 1;
  object.manifest.payload_digest = Sha256(object.payload).value();
  object.chunks = {ByteView(object.payload)};
  auto config = Config(temporary.path());
  config.resident = {64U * 1024U * 1024U, 48U * 1024U * 1024U, 16U * 1024U * 1024U};
  config.disk_budget_bytes = 32U * 1024U * 1024U;
  config.max_object_bytes = 32U * 1024U * 1024U;
  auto opened = TieredStore::Open(config);
  ASSERT_TRUE(opened.ok());
  auto initial = opened.value()->Put(object.manifest, object.chunks);
  ASSERT_TRUE(initial.ok());
  ASSERT_TRUE(opened.value()->Evict(object.manifest).ok());
  initial = Status{StatusCode::kNotFound, "release"};
  Result<TieredResidentHandle> leader(Status{StatusCode::kInternal, "not run"});
  std::jthread loader([&] { leader = opened.value()->Lookup(object.manifest); });
  while (opened.value()->State(object.manifest).value() != TierState::kLoading) {
    std::this_thread::yield();
  }
  std::stop_source source;
  source.request_stop();
  const auto waiter =
      opened.value()->Lookup(object.manifest, TieredStore::Deadline::max(), source.get_token());
  EXPECT_EQ(waiter.status().code(), StatusCode::kCancelled);
  loader.join();
  EXPECT_TRUE(leader.ok());
  EXPECT_EQ(opened.value()->State(object.manifest).value(), TierState::kResident);
  EXPECT_EQ(opened.value()->Stats().disk_read_count, 1U);
}

TEST(KvCacheTieredStoreTest, ConcurrentPromotionIsCoalescedIntoOneObjectRead) {
  TemporaryDirectory temporary;
  ObjectFixture object;
  auto opened = TieredStore::Open(Config(temporary.path()));
  ASSERT_TRUE(opened.ok());
  ASSERT_TRUE(opened.value()->Put(object.manifest, object.chunks).ok());
  ASSERT_TRUE(opened.value()->Evict(object.manifest).ok());
  constexpr std::size_t kReaders = 8;
  std::barrier start(static_cast<std::ptrdiff_t>(kReaders));
  std::array<StatusCode, kReaders> results{};
  std::vector<std::jthread> threads;
  for (std::size_t index = 0; index < kReaders; ++index) {
    threads.emplace_back([&, index] {
      start.arrive_and_wait();
      const auto result = opened.value()->Lookup(object.manifest);
      results[index] = result.ok() ? StatusCode::kOk : result.status().code();
    });
  }
  threads.clear();
  EXPECT_TRUE(
      std::ranges::all_of(results, [](StatusCode code) { return code == StatusCode::kOk; }));
  EXPECT_EQ(opened.value()->Stats().disk_read_count, 1U);
}

TEST(KvCacheTieredStoreTest, ResidentHandlePinsMemoryAcrossEvictionAndDelete) {
  TemporaryDirectory temporary;
  ObjectFixture object;
  auto opened = TieredStore::Open(Config(temporary.path()));
  ASSERT_TRUE(opened.ok());
  std::uint64_t used = 0;
  {
    auto result = opened.value()->Put(object.manifest, object.chunks);
    ASSERT_TRUE(result.ok());
    used = opened.value()->Stats().resident.used_bytes;
    ASSERT_GT(used, 0U);
    TieredResidentHandle original = std::move(result.value());
    TieredResidentHandle moved = std::move(original);
    EXPECT_EQ(original.chunk_count(), 2U);
    ASSERT_TRUE(opened.value()->Evict(object.manifest).ok());
    ASSERT_TRUE(opened.value()->Delete(object.manifest).ok());
    EXPECT_TRUE(std::ranges::equal(moved.Chunk(1).value(), object.chunks[1]));
    EXPECT_EQ(opened.value()->Stats().resident.used_bytes, used);
  }
  EXPECT_EQ(opened.value()->Stats().resident.used_bytes, used);
}

TEST(KvCacheTieredStoreTest, OpenRemovesUnpublishedTemporaryObjects) {
  TemporaryDirectory temporary;
  const auto incomplete = temporary.path() / "objects" / "aa" / std::string(64, 'a');
  std::filesystem::create_directories(incomplete);
  {
    std::ofstream output(incomplete / "chunk.tmp", std::ios::binary);
    output << "partial";
  }
  auto opened = TieredStore::Open(Config(temporary.path()));
  ASSERT_TRUE(opened.ok());
  EXPECT_FALSE(std::filesystem::exists(incomplete));
  EXPECT_EQ(opened.value()->Stats().object_count, 0U);
}

}  // namespace
}  // namespace kvstore::kvcache
