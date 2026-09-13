#include "kvstore/integration/vllm/adapter.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>

#include "kvstore/kvcache/tiered_store.hpp"

namespace kvstore::integration::vllm {
namespace {
Options Baseline() {
  return {"tenant", "060db6499f32faf8b98477b0a26969ef7d8b9987",
          "060db6499f32faf8b98477b0a26969ef7d8b9987", std::string(kVersion)};
}
Request Prompt(std::uint32_t count = 32) {
  Request r;
  r.id = "prefill-1";
  r.tokens.resize(count);
  std::iota(r.tokens.begin(), r.tokens.end(), 1U);
  return r;
}
BlockTable Blocks() { return {{3, 1, 4}, 8, 0}; }

// Models a completed host-to-device install, not a vLLM worker or GPU allocator.
class MockInjector final : public HitInjector {
 public:
  std::size_t calls{};
  std::optional<Hit> installed;
  bool fail{};
  Status Install(const Hit& hit, const Request& request) override {
    ++calls;
    if (request.stop.stop_requested()) return {StatusCode::kCancelled, "copy cancelled"};
    if (fail) return {StatusCode::kIoError, "mock H2D failure"};
    installed = hit;
    return Status::Ok();
  }
};

class ObservedTransport final : public Transport {
 public:
  SessionTransport inner;
  std::function<void(v1::Request&)> before;
  std::function<void(const v1::Request&, v1::Response&)> after;
  bool lose_commit_ack{};
  std::size_t exchanges{};
  explicit ObservedTransport(kvcache::ChunkRegistry& registry, kvcache::MatchIndex& index)
      : inner(registry, index, "tenant") {}
  Result<Bytes> Exchange(ByteView bytes) override {
    ++exchanges;
    auto request = Codec::DecodeRequest(bytes);
    if (!request.ok()) return request.status();
    if (before) before(request.value());
    auto encoded = Codec::EncodeRequest(request.value());
    if (!encoded.ok()) return encoded.status();
    auto wire = inner.Exchange(encoded.value());
    if (!wire.ok()) return wire.status();
    if (lose_commit_ack && request.value().operation() == v1::COMMIT)
      return Status{StatusCode::kIoError, "lost commit acknowledgement"};
    auto reply = Codec::DecodeResponse(wire.value());
    if (!reply.ok()) return reply.status();
    if (after) after(request.value(), reply.value());
    return Codec::EncodeResponse(reply.value());
  }
  void Disconnect() noexcept override { inner.Disconnect(); }
};

class VllmAdapter : public ::testing::Test {
 protected:
  kvcache::ChunkRegistry registry;
  kvcache::MatchIndex index;
  MockInjector injector;
  std::unique_ptr<Adapter> adapter;
  ObservedTransport* transport{};
  void SetUp() override { NewAdapter(); }
  void NewAdapter() {
    auto wire = std::make_unique<ObservedTransport>(registry, index);
    transport = wire.get();
    adapter = std::make_unique<Adapter>(std::move(wire), Baseline());
    ASSERT_TRUE(adapter->Connect().ok());
  }
  Bytes Payload(const Request& request) {
    auto manifest = adapter->Manifest(request);
    EXPECT_TRUE(manifest.ok());
    if (!manifest.ok()) return {};
    Bytes bytes(static_cast<std::size_t>(manifest.value().payload_bytes));
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::byte>(i % 251);
    return bytes;
  }
  void Publish(const Request& request = Prompt()) {
    ASSERT_TRUE(adapter->BeginPublication(request).ok());
    auto payload = Payload(request);
    ASSERT_TRUE(adapter->HostCopyReady(adapter->copy_generation(), payload).ok());
    for (std::size_t step = 0;
         step < 40 && adapter->publication_state() != PublicationState::kPublished; ++step)
      ASSERT_TRUE(adapter->AdvancePublication().ok());
    ASSERT_EQ(adapter->publication_state(), PublicationState::kPublished);
    EXPECT_EQ(adapter->staging_bytes(), 0U);
    EXPECT_EQ(registry.Pending(), 0U);
  }
};

TEST_F(VllmAdapter, FixedBaselineAndBoundedManifest) {
  auto manifest = adapter->Manifest(Prompt());
  ASSERT_TRUE(manifest.ok());
  const auto& m = manifest.value();
  EXPECT_EQ(m.model_id, kModel);
  EXPECT_EQ(m.cache_format, kFormat);
  EXPECT_EQ(m.shape, (std::vector<std::uint64_t>{24, 2, 2, 16, 2, 64}));
  EXPECT_EQ(m.strides_bytes, (std::vector<std::uint64_t>{16384, 8192, 4096, 256, 128, 2}));
  EXPECT_EQ(m.payload_bytes, 393216U);
  EXPECT_EQ(m.dtype, kvcache::DType::kBFloat16);
  EXPECT_TRUE(kvcache::ValidateManifestIdentity(m).ok());
  EXPECT_FALSE(kvcache::ValidateManifest(m).ok());  // Query has no tensor digest yet.
  EXPECT_FALSE(adapter->Manifest(Prompt(0)).ok());
  EXPECT_FALSE(adapter->Manifest(Prompt(kMaxTokens + 1)).ok());
  auto invalid = Prompt();
  invalid.tokens[0] = 151936;
  EXPECT_FALSE(adapter->Manifest(invalid).ok());
  invalid = Prompt();
  invalid.id.clear();
  EXPECT_FALSE(adapter->Manifest(invalid).ok());
  auto options = Baseline();
  options.framework_version = "0.28.0";
  Adapter unsupported(std::make_unique<SessionTransport>(registry, index, "tenant"), options);
  EXPECT_EQ(unsupported.Connect().code(), StatusCode::kUnsupported);
  options = Baseline();
  options.model_revision = "main";
  Adapter unpinned(std::make_unique<SessionTransport>(registry, index, "tenant"), options);
  EXPECT_EQ(unpinned.Connect().code(), StatusCode::kUnsupported);
}

TEST_F(VllmAdapter, MissPartialFullAndExactLookupUseSerializedSession) {
  auto miss = adapter->Prefill(Prompt(), false, Blocks(), injector);
  EXPECT_EQ(miss.status.code(), StatusCode::kNotFound);
  EXPECT_EQ(miss.hit_tokens, 0U);
  EXPECT_EQ(miss.recompute_begin, 0U);
  EXPECT_EQ(miss.total_tokens, 32U);
  Publish();
  auto exact_miss = adapter->Prefill(Prompt(48), true, Blocks(), injector);
  EXPECT_EQ(exact_miss.status.code(), StatusCode::kNotFound);
  auto partial = adapter->Prefill(Prompt(48), false, Blocks(), injector);
  ASSERT_TRUE(partial.status.ok()) << partial.status.message();
  EXPECT_EQ(partial.hit_tokens, 32U);
  EXPECT_EQ(partial.recompute_begin, 32U);
  EXPECT_EQ(partial.total_tokens, 48U);
  ASSERT_TRUE(injector.installed.has_value());
  EXPECT_EQ(injector.installed->manifest.token_count, 32U);
  EXPECT_EQ(injector.installed->payload, Payload(Prompt()));
  EXPECT_EQ(injector.installed->destination_blocks, (std::vector<std::uint64_t>{3, 1}));
  auto full = adapter->Prefill(Prompt(), true, Blocks(), injector);
  ASSERT_TRUE(full.status.ok());
  EXPECT_EQ(full.hit_tokens, full.total_tokens);
  EXPECT_EQ(full.recompute_begin, full.total_tokens);
  EXPECT_GT(transport->exchanges, 10U);
}

TEST_F(VllmAdapter, LongestFullBlockPrefixAndUnalignedSuffix) {
  Publish(Prompt(16));
  Publish(Prompt(32));
  auto result = adapter->Prefill(Prompt(35), false, Blocks(), injector);
  ASSERT_TRUE(result.status.ok());
  EXPECT_EQ(result.hit_tokens, 32U);
  EXPECT_EQ(result.total_tokens, 35U);
  EXPECT_EQ(adapter->BeginPublication(Prompt(35)).code(), StatusCode::kUnsupported);
}

TEST_F(VllmAdapter, PublicationWaitsForCopyAndCommitAndOwnsStaging) {
  const auto request = Prompt();
  ASSERT_TRUE(adapter->BeginPublication(request).ok());
  EXPECT_EQ(adapter->BeginPublication(request).code(), StatusCode::kBusy);
  EXPECT_EQ(adapter->AdvancePublication().code(), StatusCode::kBusy);
  EXPECT_EQ(registry.Pending(), 0U);
  auto bytes = Payload(request);
  ASSERT_TRUE(adapter->HostCopyReady(adapter->copy_generation(), bytes).ok());
  std::fill(bytes.begin(), bytes.end(), std::byte{0});
  ASSERT_TRUE(adapter->AdvancePublication().ok());  // RESERVE
  EXPECT_EQ(registry.Pending(), 1U);
  EXPECT_EQ(index.Size(), 0U);
  ASSERT_TRUE(adapter->AdvancePublication().ok());  // PUT 0
  ASSERT_TRUE(adapter->AdvancePublication().ok());  // PUT 1
  EXPECT_EQ(adapter->publication_state(), PublicationState::kCommit);
  EXPECT_EQ(adapter->Prefill(request, true, Blocks(), injector).hit_tokens, 0U);
  ASSERT_TRUE(adapter->AdvancePublication().ok());
  ASSERT_TRUE(adapter->Prefill(request, true, Blocks(), injector).status.ok());
  ASSERT_TRUE(injector.installed.has_value());
  EXPECT_EQ(injector.installed->payload, Payload(request));
  EXPECT_EQ(adapter->staging_bytes(), 0U);
  EXPECT_FALSE(adapter->AdvancePublication().ok());
}

TEST_F(VllmAdapter, BlockMetadataValidationAndInjectionFailureRecomputeAll) {
  Publish();
  for (auto table : {BlockTable{{1, 1}, 8, 0}, BlockTable{{8, 1}, 8, 0}, BlockTable{{1}, 8, 0},
                     BlockTable{{1, 2}, 8, 16}}) {
    const auto result = adapter->Prefill(Prompt(), true, table, injector);
    EXPECT_FALSE(result.status.ok());
    EXPECT_EQ(result.hit_tokens, 0U);
    EXPECT_EQ(result.recompute_begin, 0U);
  }
  EXPECT_EQ(injector.calls, 0U);
  injector.fail = true;
  auto failed = adapter->Prefill(Prompt(), true, Blocks(), injector);
  EXPECT_EQ(failed.status.code(), StatusCode::kIoError);
  EXPECT_EQ(failed.recompute_begin, 0U);
  EXPECT_FALSE(injector.installed.has_value());
  injector.fail = false;
  EXPECT_TRUE(adapter->Prefill(Prompt(), true, Blocks(), injector).status.ok());
}

TEST_F(VllmAdapter, IdentityPartitionsPreventCrossRevisionOrTenantHits) {
  Publish();
  auto changed = Baseline();
  changed.model_revision = std::string(40, 'a');
  Adapter other(std::make_unique<SessionTransport>(registry, index, "tenant"), changed);
  ASSERT_TRUE(other.Connect().ok());
  EXPECT_EQ(other.Prefill(Prompt(), true, Blocks(), injector).status.code(), StatusCode::kNotFound);
  changed = Baseline();
  changed.tenant = "other";
  Adapter foreign(std::make_unique<SessionTransport>(registry, index, "other"), changed);
  ASSERT_TRUE(foreign.Connect().ok());
  EXPECT_EQ(foreign.Prefill(Prompt(), true, Blocks(), injector).status.code(),
            StatusCode::kNotFound);
  EXPECT_EQ(injector.calls, 0U);
}

TEST_F(VllmAdapter, DeadlineAndPreCancelledRequestsDoNotInjectOrReserve) {
  Publish();
  auto request = Prompt();
  request.deadline_unix_ms = 1;
  auto result = adapter->Prefill(request, true, Blocks(), injector);
  EXPECT_EQ(result.status.code(), StatusCode::kDeadlineExceeded);
  EXPECT_EQ(result.recompute_begin, 0U);
  EXPECT_EQ(adapter->BeginPublication(request).code(), StatusCode::kDeadlineExceeded);
  std::stop_source stop;
  request = Prompt();
  request.stop = stop.get_token();
  stop.request_stop();
  EXPECT_EQ(adapter->Prefill(request, true, Blocks(), injector).status.code(),
            StatusCode::kCancelled);
  EXPECT_EQ(adapter->BeginPublication(request).code(), StatusCode::kCancelled);
  EXPECT_EQ(injector.calls, 0U);
  EXPECT_EQ(registry.Pending(), 0U);
}

TEST_F(VllmAdapter, CancellationAtEveryPublicationBoundaryCleansReservations) {
  for (int completed_steps = 0; completed_steps <= 4; ++completed_steps) {
    std::stop_source stop;
    auto request = Prompt();
    request.stop = stop.get_token();
    ASSERT_TRUE(adapter->BeginPublication(request).ok());
    if (completed_steps > 0) {
      ASSERT_TRUE(adapter->HostCopyReady(adapter->copy_generation(), Payload(request)).ok());
    }
    for (int step = 1; step < completed_steps; ++step)
      ASSERT_TRUE(adapter->AdvancePublication().ok());
    stop.request_stop();
    EXPECT_EQ(adapter->AdvancePublication().code(), StatusCode::kCancelled);
    EXPECT_EQ(adapter->publication_state(), PublicationState::kCancelled);
    EXPECT_EQ(adapter->staging_bytes(), 0U);
    EXPECT_EQ(registry.Pending(), 0U);
    EXPECT_EQ(registry.Size(), 0U);
    EXPECT_EQ(registry.TrackedBytes(), 0U);
    EXPECT_TRUE(adapter->CancelPublication().ok());
  }
  Publish();
}

TEST_F(VllmAdapter, ExplicitCancellationAndDestructorCleanup) {
  ASSERT_TRUE(adapter->BeginPublication(Prompt()).ok());
  ASSERT_TRUE(adapter->HostCopyReady(adapter->copy_generation(), Payload(Prompt())).ok());
  ASSERT_TRUE(adapter->AdvancePublication().ok());
  EXPECT_EQ(registry.Pending(), 1U);
  EXPECT_TRUE(adapter->CancelPublication().ok());
  EXPECT_EQ(registry.Pending(), 0U);
  ASSERT_TRUE(adapter->BeginPublication(Prompt()).ok());
  ASSERT_TRUE(adapter->HostCopyReady(adapter->copy_generation(), Payload(Prompt())).ok());
  ASSERT_TRUE(adapter->AdvancePublication().ok());
  adapter.reset();
  EXPECT_EQ(registry.Pending(), 0U);
  EXPECT_EQ(registry.TrackedBytes(), 0U);
}

TEST_F(VllmAdapter, RemoteTimeoutAfterReserveCleansWithoutExpiredCleanupDeadline) {
  ASSERT_TRUE(adapter->BeginPublication(Prompt()).ok());
  ASSERT_TRUE(adapter->HostCopyReady(adapter->copy_generation(), Payload(Prompt())).ok());
  ASSERT_TRUE(adapter->AdvancePublication().ok());
  transport->before = [](v1::Request& request) {
    if (request.operation() == v1::PUT) request.set_deadline_unix_ms(1);
  };
  EXPECT_EQ(adapter->AdvancePublication().code(), StatusCode::kDeadlineExceeded);
  EXPECT_EQ(registry.Pending(), 0U);
  EXPECT_EQ(registry.TrackedBytes(), 0U);
  EXPECT_EQ(adapter->staging_bytes(), 0U);
  transport->before = {};
  Publish();
}

TEST_F(VllmAdapter, CancelDuringGetNeverInjectsAndReleasesLease) {
  Publish();
  std::stop_source stop;
  auto request = Prompt();
  request.stop = stop.get_token();
  transport->after = [&](const v1::Request& sent, v1::Response&) {
    if (sent.operation() == v1::GET) stop.request_stop();
  };
  auto result = adapter->Prefill(request, true, Blocks(), injector);
  EXPECT_EQ(result.status.code(), StatusCode::kCancelled);
  EXPECT_EQ(result.recompute_begin, 0U);
  EXPECT_EQ(injector.calls, 0U);
  auto manifest = adapter->Manifest(request).value();
  manifest.payload_digest = kvcache::Sha256(Payload(request)).value();
  ASSERT_TRUE(index.Erase(manifest).ok());
  ASSERT_TRUE(registry.Delete(manifest).ok());
  EXPECT_EQ(registry.TrackedBytes(), 0U);
}

TEST_F(VllmAdapter, CancellationAfterCommitKeepsReusableCacheButReleasesLease) {
  std::stop_source stop;
  auto request = Prompt();
  request.stop = stop.get_token();
  transport->after = [&](const v1::Request& sent, v1::Response&) {
    if (sent.operation() == v1::COMMIT) stop.request_stop();
  };
  ASSERT_TRUE(adapter->BeginPublication(request).ok());
  ASSERT_TRUE(adapter->HostCopyReady(adapter->copy_generation(), Payload(request)).ok());
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(adapter->AdvancePublication().ok());
  EXPECT_EQ(adapter->AdvancePublication().code(), StatusCode::kCancelled);
  EXPECT_EQ(adapter->publication_state(), PublicationState::kCancelled);
  EXPECT_EQ(registry.Pending(), 0U);
  EXPECT_EQ(registry.Size(), 1U);
  EXPECT_EQ(adapter->staging_bytes(), 0U);
  EXPECT_TRUE(adapter->Prefill(Prompt(), true, Blocks(), injector).status.ok());
}

TEST_F(VllmAdapter, LostCommitAcknowledgementClosesTransportAndReconnectLooksUp) {
  transport->lose_commit_ack = true;
  ASSERT_TRUE(adapter->BeginPublication(Prompt()).ok());
  ASSERT_TRUE(adapter->HostCopyReady(adapter->copy_generation(), Payload(Prompt())).ok());
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(adapter->AdvancePublication().ok());
  EXPECT_EQ(adapter->AdvancePublication().code(), StatusCode::kIoError);
  EXPECT_EQ(adapter->staging_bytes(), 0U);
  EXPECT_EQ(registry.Pending(), 0U);
  EXPECT_EQ(registry.Size(), 1U);
  EXPECT_FALSE(adapter->Connect().ok());
  NewAdapter();
  EXPECT_TRUE(adapter->Prefill(Prompt(), true, Blocks(), injector).status.ok());
}

TEST_F(VllmAdapter, MalformedHitResponsesAndBytesNeverReachInjector) {
  Publish();
  using Mutation = std::function<void(const v1::Request&, v1::Response&)>;
  const std::vector<Mutation> mutations{
      [](const auto& r, auto& p) {
        if (r.operation() == v1::LOOKUP) p.set_hit_tokens(33);
      },
      [](const auto& r, auto& p) {
        if (r.operation() == v1::LOOKUP) p.mutable_manifest()->set_dtype(1);
      },
      [](const auto& r, auto& p) {
        if (r.operation() == v1::LOOKUP)
          p.mutable_manifest()->set_model_revision(std::string(40, 'b'));
      },
      [](const auto& r, auto& p) {
        if (r.operation() == v1::LOOKUP)
          p.mutable_manifest()->set_token_digest(std::string(32, 'a'));
      },
      [](const auto& r, auto& p) {
        if (r.operation() == v1::LOOKUP) p.set_layer_count(1);
      },
      [](const auto& r, auto& p) {
        if (r.operation() == v1::LOOKUP) p.mutable_hit_ranges(0)->set_begin(1);
      },
      [](const auto& r, auto& p) {
        if (r.operation() == v1::LOOKUP) p.set_chunk_indices(0, 1);
      },
      [](const auto& r, auto& p) {
        if (r.operation() == v1::LOOKUP) p.set_recompute(true);
      },
      [](const auto& r, auto& p) {
        if (r.operation() == v1::LOOKUP)
          p.mutable_manifest()->set_payload_digest(std::string(32, 'b'));
      },
      [](const auto& r, auto& p) {
        if (r.operation() == v1::GET) p.set_payload("truncated");
      },
      [](const auto& r, auto& p) {
        if (r.operation() == v1::GET) p.set_payload_checksum(p.payload_checksum() ^ 1U);
      },
      [](const auto& r, auto& p) {
        if (r.operation() == v1::LOOKUP) p.mutable_trace()->set_request_id("wrong");
      },
  };
  for (const auto& mutate : mutations) {
    NewAdapter();
    transport->after = mutate;
    auto result = adapter->Prefill(Prompt(), true, Blocks(), injector);
    EXPECT_FALSE(result.status.ok());
    EXPECT_EQ(result.hit_tokens, 0U);
    EXPECT_EQ(result.recompute_begin, 0U);
    EXPECT_EQ(injector.calls, 0U);
  }
}

TEST_F(VllmAdapter, InvalidHostPayloadAndDuplicatePublicationDoNotLeak) {
  ASSERT_TRUE(adapter->BeginPublication(Prompt()).ok());
  EXPECT_EQ(adapter->HostCopyReady(adapter->copy_generation(), {}).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(adapter->staging_bytes(), 0U);
  EXPECT_EQ(registry.Pending(), 0U);
  Publish();
  ASSERT_TRUE(adapter->BeginPublication(Prompt()).ok());
  ASSERT_TRUE(adapter->HostCopyReady(adapter->copy_generation(), Payload(Prompt())).ok());
  EXPECT_EQ(adapter->AdvancePublication().code(), StatusCode::kAlreadyExists);
  EXPECT_EQ(registry.Pending(), 0U);
  EXPECT_EQ(adapter->staging_bytes(), 0U);
}

TEST_F(VllmAdapter, ServerRestartIsColdUnlessExplicitlyRehydratedFromPersistence) {
  Publish();
  auto m = adapter->Manifest(Prompt()).value();
  auto payload = Payload(Prompt());
  m.payload_digest = kvcache::Sha256(payload).value();
  std::vector<char> pattern;
  const auto name = (std::filesystem::temp_directory_path() / "kvstore-vllm-XXXXXX").string();
  pattern.assign(name.begin(), name.end());
  pattern.push_back('\0');
  const auto directory = ::mkdtemp(pattern.data());
  ASSERT_NE(directory, nullptr);
  struct TempDirectory {
    std::filesystem::path path;
    ~TempDirectory() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } temp{directory};
  kvcache::TieredStoreConfig config;
  config.directory = temp.path;
  config.resident = {8U * 1024U * 1024U, 6U * 1024U * 1024U, 4U * 1024U * 1024U};
  config.disk_budget_bytes = 16U * 1024U * 1024U;
  std::vector<ByteView> chunks;
  for (std::uint32_t i = 0; i < m.chunk_count; ++i)
    chunks.push_back(
        ByteView(payload).subspan(static_cast<std::size_t>(i) * m.chunk_bytes, m.chunk_bytes));
  {
    auto disk = kvcache::TieredStore::Open(config);
    ASSERT_TRUE(disk.ok());
    {
      auto saved = disk.value()->Put(m, chunks);
      ASSERT_TRUE(saved.ok());
    }
    ASSERT_TRUE(disk.value()->Evict(m).ok());
  }
  adapter.reset();
  kvcache::ChunkRegistry restarted_registry;
  kvcache::MatchIndex restarted_index;
  Adapter restarted(
      std::make_unique<SessionTransport>(restarted_registry, restarted_index, "tenant"),
      Baseline());
  ASSERT_TRUE(restarted.Connect().ok());
  EXPECT_EQ(restarted.Prefill(Prompt(), true, Blocks(), injector).status.code(),
            StatusCode::kNotFound);
  auto reopened = kvcache::TieredStore::Open(config);
  ASSERT_TRUE(reopened.ok());
  auto restored = reopened.value()->Lookup(m);
  ASSERT_TRUE(restored.ok());
  Bytes restored_bytes;
  for (std::size_t i = 0; i < restored.value().chunk_count(); ++i) {
    auto chunk = restored.value().Chunk(i);
    ASSERT_TRUE(chunk.ok());
    restored_bytes.insert(restored_bytes.end(), chunk.value().begin(), chunk.value().end());
  }
  EXPECT_EQ(restored_bytes, payload);
  ASSERT_TRUE(restarted.BeginPublication(Prompt()).ok());
  ASSERT_TRUE(restarted.HostCopyReady(restarted.copy_generation(), restored_bytes).ok());
  for (int i = 0; i < 4; ++i) ASSERT_TRUE(restarted.AdvancePublication().ok());
  EXPECT_TRUE(restarted.Prefill(Prompt(), true, Blocks(), injector).status.ok());
  ASSERT_TRUE(injector.installed.has_value());
  EXPECT_EQ(injector.installed->payload, payload);
}
}  // namespace
}  // namespace kvstore::integration::vllm
