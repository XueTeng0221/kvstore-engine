#include <gtest/gtest.h>
#include <array>
#include <filesystem>
#include <unistd.h>
#include <algorithm>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <future>
#include "kvstore/kvcache/request_path.hpp"

namespace kvstore::kvcache {
class RequestPathTestPeer {
 public:
  static void Pause(RequestPath& path) {
    std::lock_guard lock(path.mutex_);
    path.worker_paused_for_test_ = true;
  }
  static void Resume(RequestPath& path) {
    {
      std::lock_guard lock(path.mutex_);
      path.worker_paused_for_test_ = false;
    }
    path.work_cv_.notify_one();
  }
};
namespace {

struct Fixture {
  TensorManifest manifest;
  Bytes payload{128, std::byte{'x'}};
  std::vector<ByteView> chunks;
  Fixture() {
    manifest.tenant_id = "tenant-a";
    manifest.model_id = "model";
    manifest.model_revision = "v1";
    manifest.tokenizer_revision = "tok";
    manifest.cache_format = "test";
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
    chunks = {ByteView(payload.data(), 64), ByteView(payload.data() + 64, 64)};
  }
};

TieredStoreConfig Config(const std::filesystem::path& path) {
  return {path, {4096, 3072, 1024}, 4096, 1024, 8, 1024, {}};
}

std::filesystem::path Temp() {
  std::array<char, 32> name{};
  const std::string pattern = "/tmp/kvstore-path-XXXXXX";
  std::copy(pattern.begin(), pattern.end(), name.begin());
  return ::mkdtemp(name.data());
}

struct ReadGate {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered{};
  bool release{};
  Status operator()(std::string_view stage) {
    if (stage != "chunk.read.after-progress") return Status::Ok();
    std::unique_lock lock(mutex);
    entered = true;
    cv.notify_all();
    cv.wait(lock, [&] { return release; });
    return Status::Ok();
  }
  bool WaitEntered() {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered; });
  }
  void Release() {
    {
      std::lock_guard lock(mutex);
      release = true;
    }
    cv.notify_all();
  }
};

void MakeDiskPrefix(TieredStore& store, MatchIndex& index, Fixture& fixture) {
  ASSERT_TRUE(store.Put(fixture.manifest, fixture.chunks).ok());
  ASSERT_TRUE(store.Evict(fixture.manifest).ok());
  ASSERT_TRUE(index.Insert(fixture.manifest, CanonicalCacheKey(fixture.manifest).value()).ok());
}
}

TEST(KvCacheRequestPathTest, ExactReturnsResidentSourceAndMetrics) {
  const auto directory = Temp();
  Fixture fixture;
  auto store = TieredStore::Open(Config(directory));
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(store.value()->Put(fixture.manifest, fixture.chunks).ok());
  MatchIndex index;
  ASSERT_TRUE(index.Insert(fixture.manifest, CanonicalCacheKey(fixture.manifest).value()).ok());
  RequestPath path(index, *store.value());
  LookupRequest request;
  request.query = fixture.manifest;
  request.token_ids = {1, 2, 3, 4};
  const auto result = path.Lookup(request);
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result.value().match_kind, MatchKind::kExact);
  EXPECT_EQ(result.value().storage_kind, StorageKind::kMemory);
  EXPECT_TRUE(result.value().recompute_ranges.empty());
  EXPECT_EQ(path.Metrics().exact, 1U);
  EXPECT_EQ(path.Metrics().memory, 1U);
  path.Shutdown();
  std::filesystem::remove_all(directory);
}

TEST(KvCacheRequestPathTest, CancellationAndDeadlineAreWaiterLocal) {
  const auto directory = Temp();
  Fixture fixture;
  auto store = TieredStore::Open(Config(directory));
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(store.value()->Put(fixture.manifest, fixture.chunks).ok());
  MatchIndex index;
  ASSERT_TRUE(index.Insert(fixture.manifest, CanonicalCacheKey(fixture.manifest).value()).ok());
  RequestPath path(index, *store.value());
  std::stop_source cancelled;
  cancelled.request_stop();
  LookupRequest request;
  request.query = fixture.manifest;
  request.token_ids = {1, 2, 3, 4};
  request.cancel = cancelled.get_token();
  EXPECT_EQ(path.Lookup(request).status().code(), StatusCode::kCancelled);
  request.cancel = {};
  request.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  EXPECT_EQ(path.Lookup(request).status().code(), StatusCode::kDeadlineExceeded);
  EXPECT_EQ(path.Metrics().miss, 0U);
  path.Shutdown();
  std::filesystem::remove_all(directory);
}

TEST(KvCacheRequestPathTest, ShutdownCompletesWaitingOperation) {
  const auto directory = Temp();
  Fixture fixture;
  auto store = TieredStore::Open(Config(directory));
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(store.value()->Put(fixture.manifest, fixture.chunks).ok());
  MatchIndex index;
  ASSERT_TRUE(index.Insert(fixture.manifest, CanonicalCacheKey(fixture.manifest).value()).ok());
  RequestPath path(index, *store.value());
  LookupRequest request;
  request.query = fixture.manifest;
  request.token_ids = {1, 2, 3, 4};
  auto result = path.Lookup(request);
  ASSERT_TRUE(result.ok());
  path.Shutdown();
  EXPECT_EQ(path.Lookup(request).status().code(), StatusCode::kCancelled);
  std::filesystem::remove_all(directory);
}

TEST(KvCacheRequestPathTest, PrefixReturnsSourceAndDeterministicRecomputePlan) {
  const auto directory = Temp();
  Fixture fixture;
  auto source = fixture.manifest;
  const std::array<std::uint32_t, 2> source_tokens{1, 2};
  source.token_count = source_tokens.size();
  source.token_digest = TokenDigest(source_tokens).value();
  source.shape[2] = 2;
  source.strides_bytes[0] = 32;
  source.strides_bytes[1] = 32;
  source.payload_bytes = 64;
  source.chunk_count = 1;
  source.payload_digest = Sha256(ByteView(fixture.payload.data(), 64)).value();
  std::vector<ByteView> source_chunks{ByteView(fixture.payload.data(), 64)};
  auto store = TieredStore::Open(Config(directory)); ASSERT_TRUE(store.ok());
  auto source_put = store.value()->Put(source, source_chunks);
  ASSERT_TRUE(source_put.ok()) << source_put.status().message();
  ASSERT_TRUE(store.value()->Evict(source).ok());
  MatchIndex index; ASSERT_TRUE(index.Insert(source, CanonicalCacheKey(source).value()).ok());
  RequestPath path(index, *store.value());
  LookupRequest request; request.query = fixture.manifest; request.token_ids = {1, 2, 3, 4};
  request.prefix_lengths = {2};
  auto result = path.Lookup(request);
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result.value().match_kind, MatchKind::kPrefix);
  ASSERT_EQ(result.value().recompute_ranges.size(), 1U);
  EXPECT_EQ(result.value().recompute_ranges[0], (TensorRange{2, 4, 0, 1}));
  EXPECT_EQ(result.value().storage_kind, StorageKind::kDisk);
  EXPECT_EQ(path.Metrics().prefix, 1U); EXPECT_EQ(path.Metrics().disk, 1U);
  path.Shutdown(); std::filesystem::remove_all(directory);
}

TEST(KvCacheRequestPathTest, CoalescesDiskReadAndRecordsMetrics) {
  const auto directory = Temp(); Fixture fixture; ReadGate gate;
  auto config = Config(directory); config.io_fault = std::ref(gate);
  auto store = TieredStore::Open(config); ASSERT_TRUE(store.ok());
  MatchIndex index; MakeDiskPrefix(*store.value(), index, fixture);
  RequestPath path(index, *store.value()); LookupRequest request;
  request.query = fixture.manifest; request.token_ids = {1, 2, 3, 4};
  std::promise<Result<LookupResult>> first_promise;
  auto first = std::async(std::launch::async, [&, request] { return path.Lookup(request); });
  ASSERT_TRUE(gate.WaitEntered());
  auto second = std::async(std::launch::async, [&, request] { return path.Lookup(request); });
  for (int attempt = 0; attempt < 200 && path.Metrics().coalesced < 1; ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_EQ(path.Metrics().coalesced, 1U);
  gate.Release();
  ASSERT_TRUE(first.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  ASSERT_TRUE(second.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  EXPECT_TRUE(first.get().ok()); EXPECT_TRUE(second.get().ok());
  EXPECT_EQ(path.Metrics().coalesced, 1U); EXPECT_EQ(path.Metrics().disk, 2U);
  path.Shutdown(); std::filesystem::remove_all(directory);
}

TEST(KvCacheRequestPathTest, EarlyDeadlineDoesNotCancelLongerWaiter) {
  const auto directory = Temp(); Fixture fixture; ReadGate gate;
  auto config = Config(directory); config.io_fault = std::ref(gate);
  auto store = TieredStore::Open(config); ASSERT_TRUE(store.ok()); MatchIndex index;
  MakeDiskPrefix(*store.value(), index, fixture); RequestPath path(index, *store.value());
  LookupRequest short_request; short_request.query = fixture.manifest; short_request.token_ids = {1,2,3,4};
  short_request.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
  LookupRequest long_request = short_request; long_request.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  auto short_waiter = std::async(std::launch::async, [&, short_request] { return path.Lookup(short_request); });
  ASSERT_TRUE(gate.WaitEntered()); auto long_waiter = std::async(std::launch::async, [&, long_request] { return path.Lookup(long_request); });
  EXPECT_EQ(short_waiter.get().status().code(), StatusCode::kDeadlineExceeded);
  gate.Release(); ASSERT_TRUE(long_waiter.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  EXPECT_TRUE(long_waiter.get().ok()); path.Shutdown(); std::filesystem::remove_all(directory);
}

TEST(KvCacheRequestPathTest, LastWaiterCancelsAndShutdownIsRepeatable) {
  const auto directory = Temp(); Fixture fixture; ReadGate gate; auto config = Config(directory); config.io_fault = std::ref(gate);
  auto store = TieredStore::Open(config); ASSERT_TRUE(store.ok()); MatchIndex index; MakeDiskPrefix(*store.value(), index, fixture); RequestPath path(index, *store.value());
  LookupRequest request; request.query = fixture.manifest; request.token_ids = {1,2,3,4};
  auto waiter = std::async(std::launch::async, [&, request] { return path.Lookup(request); }); ASSERT_TRUE(gate.WaitEntered());
  std::stop_source stop; stop.request_stop(); auto cancelled_request = request; cancelled_request.cancel = stop.get_token();
  EXPECT_EQ(path.Lookup(cancelled_request).status().code(), StatusCode::kCancelled); gate.Release();
  ASSERT_TRUE(waiter.wait_for(std::chrono::seconds(2)) == std::future_status::ready); waiter.get();
  path.Shutdown(); path.Shutdown(); std::filesystem::remove_all(directory);
}

struct SchedulerFailureCase {
  const char* name;
  bool SchedulerConfig::*flag;
  StatusCode expected;
};

class KvCacheRequestPathSchedulerFailureTest
    : public ::testing::TestWithParam<SchedulerFailureCase> {};

TEST_P(KvCacheRequestPathSchedulerFailureTest, FailureDoesNotStickAndRetrySucceeds) {
  const auto directory = Temp();
  Fixture fixture;
  auto store = TieredStore::Open(Config(directory));
  ASSERT_TRUE(store.ok());
  MatchIndex index;
  MakeDiskPrefix(*store.value(), index, fixture);

  SchedulerConfig scheduler_config;
  scheduler_config.*GetParam().flag = true;
  RequestPath path(index, *store.value(), scheduler_config);
  LookupRequest request;
  request.query = fixture.manifest;
  request.token_ids = {1, 2, 3, 4};
  request.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

  auto failed = std::async(std::launch::async, [&path, request] { return path.Lookup(request); });
  ASSERT_EQ(failed.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  const auto failure = failed.get();
  ASSERT_FALSE(failure.ok());
  EXPECT_EQ(failure.status().code(), GetParam().expected);

  const auto after_failure = path.SchedulerState();
  EXPECT_EQ(after_failure.pending, 0U);
  EXPECT_EQ(after_failure.inflight_bytes, 0U);

  auto retry = std::async(std::launch::async, [&path, request] { return path.Lookup(request); });
  ASSERT_EQ(retry.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  ASSERT_TRUE(retry.get().ok());
  const auto after_retry = path.SchedulerState();
  EXPECT_EQ(after_retry.pending, 0U);
  EXPECT_EQ(after_retry.inflight_bytes, 0U);
  path.Shutdown();
  std::filesystem::remove_all(directory);
}

TEST(KvCacheRequestPathTest, ShutdownRacingCompletionCompletesFutureExactlyOnce) {
  const auto directory = Temp(); Fixture fixture; ReadGate gate;
  auto config = Config(directory); config.io_fault = std::ref(gate);
  auto store = TieredStore::Open(config); ASSERT_TRUE(store.ok());
  MatchIndex index; MakeDiskPrefix(*store.value(), index, fixture);
  RequestPath path(index, *store.value());
  LookupRequest request; request.query = fixture.manifest; request.token_ids = {1,2,3,4};
  auto future = std::async(std::launch::async, [&path, request] { return path.Lookup(request); });
  ASSERT_TRUE(gate.WaitEntered());
  std::thread shutdown([&path] { path.Shutdown(); });
  gate.Release();
  ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  auto result = future.get();
  EXPECT_TRUE(result.ok() || result.status().code() == StatusCode::kCancelled);
  shutdown.join();
  path.Shutdown();
  EXPECT_EQ(path.SchedulerState().pending, 0U);
  EXPECT_EQ(path.SchedulerState().inflight_bytes, 0U);
  std::filesystem::remove_all(directory);
}

TEST(KvCacheRequestPathTest, PopFailureDrainsDistinctPendingOperationsAndRetrySucceeds) {
  const auto directory = Temp(); Fixture fixture; ReadGate gate;
  auto config = Config(directory); config.io_fault = std::ref(gate);
  auto store = TieredStore::Open(config); ASSERT_TRUE(store.ok());
  MatchIndex index; MakeDiskPrefix(*store.value(), index, fixture);
  SchedulerConfig scheduler; scheduler.fail_pop = true;
  RequestPath path(index, *store.value(), scheduler);
  RequestPathTestPeer::Pause(path);
  LookupRequest a; a.query = fixture.manifest; a.token_ids = {1,2,3,4};
  auto altered = fixture.manifest; altered.model_revision = "v2";
  ASSERT_TRUE(store.value()->Put(altered, fixture.chunks).ok());
  ASSERT_TRUE(store.value()->Evict(altered).ok());
  ASSERT_TRUE(index.Insert(altered, CanonicalCacheKey(altered).value()).ok());
  LookupRequest b = a; b.query = altered;
  auto first = std::async(std::launch::async, [&path, a] { return path.Lookup(a); });
  auto second = std::async(std::launch::async, [&path, b] { return path.Lookup(b); });
  for (int attempt = 0; attempt < 200 && path.SchedulerState().pending < 2; ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_EQ(path.SchedulerState().pending, 2U);
  RequestPathTestPeer::Resume(path);
  gate.Release();
  ASSERT_EQ(first.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  ASSERT_EQ(second.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_EQ(first.get().status().code(), StatusCode::kInternal);
  EXPECT_EQ(second.get().status().code(), StatusCode::kInternal);
  EXPECT_EQ(path.SchedulerState().pending, 0U);
  EXPECT_EQ(path.SchedulerState().inflight_bytes, 0U);
  auto retry_a = std::async(std::launch::async, [&path, a] { return path.Lookup(a); });
  auto retry_b = std::async(std::launch::async, [&path, b] { return path.Lookup(b); });
  ASSERT_EQ(retry_a.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  ASSERT_EQ(retry_b.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_TRUE(retry_a.get().ok());
  EXPECT_TRUE(retry_b.get().ok());
  path.Shutdown(); std::filesystem::remove_all(directory);
}

TEST(KvCacheRequestPathTest, CompleteFailureIsSharedByCoalescedWaitersAndRetrySucceeds) {
  const auto directory = Temp(); Fixture fixture; ReadGate gate;
  auto config = Config(directory); config.io_fault = std::ref(gate);
  auto store = TieredStore::Open(config); ASSERT_TRUE(store.ok()); MatchIndex index;
  MakeDiskPrefix(*store.value(), index, fixture);
  SchedulerConfig scheduler; scheduler.fail_complete = true;
  RequestPath path(index, *store.value(), scheduler); LookupRequest request;
  request.query = fixture.manifest; request.token_ids = {1,2,3,4};
  auto first = std::async(std::launch::async, [&path, request] { return path.Lookup(request); });
  ASSERT_TRUE(gate.WaitEntered());
  auto second = std::async(std::launch::async, [&path, request] { return path.Lookup(request); });
  for (int attempt = 0; attempt < 200 && path.Metrics().coalesced == 0; ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_EQ(path.Metrics().coalesced, 1U);
  gate.Release();
  ASSERT_EQ(first.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  ASSERT_EQ(second.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_EQ(first.get().status().code(), StatusCode::kInternal);
  EXPECT_EQ(second.get().status().code(), StatusCode::kInternal);
  EXPECT_TRUE(path.Lookup(request).ok());
  path.Shutdown(); std::filesystem::remove_all(directory);
}

INSTANTIATE_TEST_SUITE_P(
    OneShotFailures, KvCacheRequestPathSchedulerFailureTest,
    ::testing::Values(SchedulerFailureCase{"fail_submit", &SchedulerConfig::fail_submit,
                                           StatusCode::kInternal},
                      SchedulerFailureCase{"fail_pop", &SchedulerConfig::fail_pop,
                                           StatusCode::kInternal},
                      SchedulerFailureCase{"fail_complete", &SchedulerConfig::fail_complete,
                                           StatusCode::kInternal}),
    [](const ::testing::TestParamInfo<SchedulerFailureCase>& info) { return info.param.name; });
}
