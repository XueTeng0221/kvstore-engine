#include <gtest/gtest.h>

#include <array>
#include <future>
#include <type_traits>

#include "kvstore/common/crc32.hpp"
#include "kvstore/integration/protocol.hpp"

namespace kvstore::integration {
class SessionTestPeer {
 public:
  static void FailTracking(Session& s) {
    std::lock_guard lock(s.mutex_);
    s.fail_tracking_ = true;
  }
};
namespace {
static_assert(!std::is_copy_constructible_v<Session> && !std::is_move_constructible_v<Session>);
kvcache::TensorManifest Manifest(std::uint32_t count = 4) {
  kvcache::TensorManifest m;
  m.tenant_id = "tenant";
  m.model_id = "m";
  m.model_revision = "rev";
  m.tokenizer_revision = "tok";
  m.cache_format = "x";
  m.adapter_id = "adapter";
  m.adapter_revision = "adapter-rev";
  m.token_count = count;
  m.layer_count = 1;
  std::vector<std::uint32_t> tokens;
  for (std::uint32_t i = 1; i <= count; ++i) tokens.push_back(i);
  m.token_digest = kvcache::TokenDigest(tokens).value();
  m.shape = {2, 1, count, 2, 4};
  m.axis_order = {kvcache::TensorAxis::kKeyValue, kvcache::TensorAxis::kLayer,
                  kvcache::TensorAxis::kToken, kvcache::TensorAxis::kHead,
                  kvcache::TensorAxis::kHeadDimension};
  m.strides_bytes = {16U * count, 16U * count, 16, 8, 2};
  m.payload_bytes = 32U * count;
  m.chunk_bytes = 128;
  m.chunk_count = (count + 3) / 4;
  m.created_at_ns = 17;
  m.accessed_at_ns = 19;
  m.payload_digest =
      kvcache::Sha256(Bytes(static_cast<std::size_t>(m.payload_bytes), std::byte{'x'})).value();
  return m;
}
v1::Request Request(v1::Operation operation, const std::string& tenant = "tenant") {
  v1::Request r;
  r.set_operation(operation);
  r.mutable_trace()->set_request_id("r");
  r.mutable_trace()->set_model_id("m");
  r.mutable_trace()->set_tenant_id(tenant);
  return r;
}
v1::Response Call(Session& s, const v1::Request& r) {
  auto bytes = Codec::EncodeRequest(r);
  if (!bytes.ok()) throw std::runtime_error(std::string(bytes.status().message()));
  auto reply = s.Exchange(bytes.value());
  if (!reply.ok()) throw std::runtime_error(std::string(reply.status().message()));
  auto decoded = Codec::DecodeResponse(reply.value());
  if (!decoded.ok()) throw std::runtime_error(std::string(decoded.status().message()));
  EXPECT_EQ(decoded.value().operation(), r.operation());
  EXPECT_EQ(decoded.value().trace().SerializeAsString(), r.trace().SerializeAsString());
  return std::move(decoded.value());
}
void Negotiate(Session& s, const std::string& tenant = "tenant") {
  auto r = Request(v1::NEGOTIATE, tenant);
  r.mutable_capabilities()->set_major(kMajor);
  r.mutable_capabilities()->set_pinned_cpu(true);
  r.mutable_capabilities()->set_cuda_ipc(true);
  auto response = Call(s, r);
  ASSERT_EQ(response.status(), v1::OK);
  EXPECT_TRUE(response.capabilities().pinned_cpu());
  EXPECT_FALSE(response.capabilities().cuda_ipc());
}
class IntegrationSession : public ::testing::Test {
 protected:
  kvcache::ChunkRegistry registry;
  kvcache::MatchIndex index;
  Session s{registry, index, "tenant", "m"};
  void SetUp() override { Negotiate(s); }
  std::uint64_t Reserve(Session& target, kvcache::TensorManifest m = Manifest()) {
    auto r = Request(v1::RESERVE);
    *r.mutable_manifest() = ToProto(m);
    auto response = Call(target, r);
    EXPECT_EQ(response.status(), v1::OK) << response.message();
    return response.reservation_id();
  }
  v1::Response Put(Session& target, std::uint64_t id, char byte = 'x', std::uint32_t chunk = 0) {
    auto r = Request(v1::PUT);
    r.set_reservation_id(id);
    r.set_chunk_index(chunk);
    const Bytes bytes(128, static_cast<std::byte>(byte));
    r.set_payload(bytes.data(), bytes.size());
    r.set_payload_checksum(Crc32(bytes));
    return Call(target, r);
  }
  v1::Response Op(Session& target, v1::Operation op, std::uint64_t id) {
    auto r = Request(op);
    r.set_reservation_id(id);
    r.set_lease_id(id);
    return Call(target, r);
  }
  std::uint64_t Publish() {
    auto id = Reserve(s);
    EXPECT_EQ(Put(s, id).status(), v1::OK);
    auto result = Op(s, v1::COMMIT, id);
    EXPECT_EQ(result.status(), v1::OK) << result.message();
    return result.lease_id();
  }
  v1::Request Lookup(std::uint32_t count = 4, bool exact = true) {
    auto r = Request(v1::LOOKUP);
    *r.mutable_manifest() = ToProto(Manifest(count));
    for (std::uint32_t i = 1; i <= count; ++i) r.add_token_ids(i);
    r.set_exact(exact);
    r.add_prefix_lengths(4);
    if (count > 4) r.add_prefix_lengths(count);
    return r;
  }
};
TEST(IntegrationCodec, EverySplitAndCoalescedFrames) {
  auto request = Request(v1::ABORT);
  request.set_reservation_id(1);
  auto e = Codec::EncodeRequest(request);
  ASSERT_TRUE(e.ok());
  for (std::size_t split = 0; split <= e.value().size(); ++split) {
    Bytes buffer;
    auto a = Codec::Feed(buffer, ByteView(e.value()).first(split));
    ASSERT_TRUE(a.ok());
    auto b = Codec::Feed(buffer, ByteView(e.value()).subspan(split));
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(a.value().size() + b.value().size(), 1U);
    EXPECT_TRUE(buffer.empty());
  }
  Bytes input = e.value();
  input.insert(input.end(), e.value().begin(), e.value().end());
  Bytes buffer;
  auto result = Codec::Feed(buffer, input);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().size(), 2U);
}
TEST(IntegrationCodec, ExactMaximumAndAdmissionBeforeAllocation) {
  Frame f{2, kMajor, kMinor, Bytes(Codec::kMaxFrame - Codec::kHeader)};
  auto e = Codec::Encode(f);
  ASSERT_TRUE(e.ok());
  EXPECT_EQ(e.value().size(), Codec::kMaxFrame);
  EXPECT_TRUE(Codec::Decode(e.value()).ok());
  f.payload.push_back(std::byte{});
  EXPECT_FALSE(Codec::Encode(f).ok());
  Bytes buffer{std::byte{'K'}};
  const auto capacity = buffer.capacity();
  auto rejected = Codec::Feed(buffer, e.value());
  EXPECT_FALSE(rejected.ok());
  EXPECT_EQ(buffer.size(), 1U);
  EXPECT_EQ(buffer.capacity(), capacity);
  Bytes enormous(Codec::kMaxFrame + 1);
  buffer.clear();
  EXPECT_FALSE(Codec::Feed(buffer, enormous).ok());
  EXPECT_TRUE(buffer.empty());
}
TEST(IntegrationCodec, MalformedTruncatedVersionAndDirection) {
  auto e = Codec::EncodeRequest(Request(v1::GET));
  ASSERT_TRUE(e.ok());
  for (std::size_t size = 0; size < e.value().size(); ++size)
    EXPECT_FALSE(Codec::DecodeRequest(ByteView(e.value()).first(size)).ok());
  auto bad = e.value();
  bad[0] = std::byte{};
  EXPECT_FALSE(Codec::DecodeRequest(bad).ok());
  bad = e.value();
  bad[6] = std::byte{2};
  EXPECT_FALSE(Codec::DecodeRequest(bad).ok());
  bad = e.value();
  bad[8] = std::byte{1};
  EXPECT_FALSE(Codec::DecodeRequest(bad).ok());
  bad = e.value();
  bad[4] = std::byte{4};
  EXPECT_FALSE(Codec::DecodeRequest(bad).ok());
  EXPECT_FALSE(Codec::DecodeResponse(e.value()).ok());
  Frame malformed{5, kMajor, kMinor, {std::byte{0xff}}};
  EXPECT_FALSE(Codec::DecodeRequest(Codec::Encode(malformed).value()).ok());
  bad = e.value();
  for (std::size_t i = 9; i < 13; ++i) bad[i] = std::byte{0xff};
  Bytes buffer;
  EXPECT_FALSE(Codec::Feed(buffer, bad).ok());
  auto unknown = Request(static_cast<v1::Operation>(255));
  EXPECT_FALSE(Codec::EncodeRequest(unknown).ok());
}
TEST(IntegrationCodec, FullManifestRoundTripAndNarrowingRejection) {
  auto m = Manifest();
  auto p = ToProto(m);
  auto roundtrip = FromProto(p);
  ASSERT_TRUE(roundtrip.ok());
  EXPECT_EQ(roundtrip.value(), m);
  p.set_dtype(257);
  EXPECT_FALSE(FromProto(p).ok());
  p = ToProto(m);
  p.set_version(65537);
  EXPECT_FALSE(FromProto(p).ok());
  p = ToProto(m);
  p.set_token_digest("short");
  EXPECT_FALSE(FromProto(p).ok());
  p = ToProto(m);
  p.set_axis_order(0, 257);
  EXPECT_FALSE(FromProto(p).ok());
  p = ToProto(m);
  p.set_device_kind(257);
  EXPECT_FALSE(FromProto(p).ok());
}
TEST_F(IntegrationSession, NegotiationIdentityAndRenegotiation) {
  Session fresh(registry, index, "tenant", "m");
  EXPECT_EQ(Call(fresh, Request(v1::RESERVE)).status(), v1::UNSUPPORTED);
  auto r = Request(v1::NEGOTIATE);
  r.mutable_capabilities()->set_major(2);
  r.mutable_capabilities()->set_pinned_cpu(true);
  EXPECT_EQ(Call(fresh, r).status(), v1::UNSUPPORTED);
  r.mutable_capabilities()->set_major(1);
  r.mutable_trace()->set_tenant_id("foreign");
  EXPECT_EQ(Call(fresh, r).status(), v1::INVALID_ARGUMENT);
  EXPECT_EQ(Call(s, Request(v1::NEGOTIATE)).status(), v1::ALREADY_EXISTS);
}
TEST_F(IntegrationSession, ForeignReservationsCannotPutCommitAbortOrCancel) {
  auto id = Reserve(s);
  Session same(registry, index, "tenant", "m");
  Negotiate(same);
  EXPECT_EQ(Put(same, id).status(), v1::NOT_FOUND);
  for (auto op : {v1::COMMIT, v1::ABORT, v1::CANCEL})
    EXPECT_EQ(Op(same, op, id).status(), v1::NOT_FOUND);
  Session foreign(registry, index, "foreign", "m");
  Negotiate(foreign, "foreign");
  for (auto op : {v1::PUT, v1::COMMIT, v1::ABORT, v1::CANCEL}) {
    auto r = Request(op, "foreign");
    r.set_reservation_id(id);
    EXPECT_EQ(Call(foreign, r).status(), v1::NOT_FOUND);
  }
  EXPECT_EQ(registry.Pending(), 1U);
  EXPECT_EQ(Put(s, id).status(), v1::OK);
  EXPECT_EQ(Op(s, v1::COMMIT, id).status(), v1::OK);
}
TEST_F(IntegrationSession, ForeignManifestAndLeaseRejected) {
  auto lease = Publish();
  Session foreign(registry, index, "foreign", "m");
  Negotiate(foreign, "foreign");
  auto lookup = Lookup();
  lookup.mutable_trace()->set_tenant_id("foreign");
  EXPECT_EQ(Call(foreign, lookup).status(), v1::INVALID_ARGUMENT);
  lookup.set_operation(v1::RESERVE);
  EXPECT_EQ(Call(foreign, lookup).status(), v1::INVALID_ARGUMENT);
  for (auto op : {v1::GET, v1::RELEASE}) {
    auto r = Request(op, "foreign");
    r.set_lease_id(lease);
    EXPECT_EQ(Call(foreign, r).status(), v1::NOT_FOUND);
  }
  Session same(registry, index, "tenant", "m");
  Negotiate(same);
  EXPECT_EQ(Op(same, v1::GET, lease).status(), v1::NOT_FOUND);
  EXPECT_EQ(Op(same, v1::RELEASE, lease).status(), v1::NOT_FOUND);
  EXPECT_EQ(Op(s, v1::GET, lease).payload(), std::string(128, 'x'));
}
TEST_F(IntegrationSession, IncompleteCommitRetainsTrackingAndRetrySucceeds) {
  auto id = Reserve(s);
  const auto bytes = registry.TrackedBytes();
  EXPECT_EQ(Op(s, v1::COMMIT, id).status(), v1::BUSY);
  EXPECT_EQ(index.Size(), 0U);
  EXPECT_EQ(registry.Pending(), 1U);
  EXPECT_EQ(registry.TrackedBytes(), bytes);
  EXPECT_EQ(Put(s, id).status(), v1::OK);
  auto commit = Op(s, v1::COMMIT, id);
  EXPECT_EQ(commit.status(), v1::OK);
  EXPECT_NE(commit.lease_id(), 0U);
  EXPECT_EQ(registry.Pending(), 0U);
  EXPECT_EQ(index.Size(), 1U);
  EXPECT_EQ(Op(s, v1::COMMIT, id).status(), v1::NOT_FOUND);
}
TEST_F(IntegrationSession, DigestFailureAbortsAndDestructorCleans) {
  {
    Session temp(registry, index, "tenant", "m");
    Negotiate(temp);
    auto id = Reserve(temp);
    EXPECT_EQ(Put(temp, id, 'y').status(), v1::OK);
    EXPECT_EQ(Op(temp, v1::COMMIT, id).status(), v1::CORRUPTION);
    EXPECT_EQ(registry.Pending(), 1U);
    EXPECT_EQ(index.Size(), 0U);
  }
  EXPECT_EQ(registry.Pending(), 0U);
  EXPECT_EQ(registry.TrackedBytes(), 0U);
  auto id = Reserve(s);
  EXPECT_EQ(Op(s, v1::ABORT, id).status(), v1::OK);
  EXPECT_EQ(Op(s, v1::ABORT, id).status(), v1::NOT_FOUND);
}
TEST_F(IntegrationSession, TrackingAllocationRollbackAndRetry) {
  SessionTestPeer::FailTracking(s);
  auto r = Request(v1::RESERVE);
  *r.mutable_manifest() = ToProto(Manifest());
  EXPECT_EQ(Call(s, r).status(), v1::LIMIT_EXCEEDED);
  EXPECT_EQ(registry.Pending(), 0U);
  EXPECT_EQ(registry.TrackedBytes(), 0U);
  auto id = Reserve(s);
  EXPECT_EQ(Op(s, v1::CANCEL, id).status(), v1::OK);
  EXPECT_EQ(registry.Pending(), 0U);
}
TEST_F(IntegrationSession, LeaseReleaseUnpinsWithoutDeleting) {
  auto lease = Publish();
  EXPECT_EQ(Op(s, v1::RELEASE, lease).status(), v1::OK);
  EXPECT_EQ(registry.Size(), 1U);
  EXPECT_EQ(Op(s, v1::GET, lease).status(), v1::NOT_FOUND);
  auto a = Call(s, Lookup());
  auto b = Call(s, Lookup());
  ASSERT_EQ(a.status(), v1::OK);
  ASSERT_EQ(b.status(), v1::OK);
  EXPECT_NE(a.lease_id(), b.lease_id());
  ASSERT_TRUE(registry.Delete(Manifest()).ok());
  EXPECT_GT(registry.TrackedBytes(), 0U);
  EXPECT_EQ(Op(s, v1::RELEASE, a.lease_id()).status(), v1::OK);
  EXPECT_GT(registry.TrackedBytes(), 0U);
  EXPECT_EQ(Op(s, v1::GET, b.lease_id()).payload(), std::string(128, 'x'));
  EXPECT_EQ(Op(s, v1::RELEASE, b.lease_id()).status(), v1::OK);
  EXPECT_EQ(registry.TrackedBytes(), 0U);
  EXPECT_EQ(Op(s, v1::RELEASE, b.lease_id()).status(), v1::NOT_FOUND);
}
TEST_F(IntegrationSession, DisconnectIsTerminalAndReleasesAllPins) {
  Publish();
  ASSERT_TRUE(registry.Delete(Manifest()).ok());
  EXPECT_GT(registry.TrackedBytes(), 0U);
  s.Disconnect();
  s.Disconnect();
  EXPECT_EQ(registry.TrackedBytes(), 0U);
  EXPECT_EQ(Call(s, Request(v1::NEGOTIATE)).status(), v1::CANCELLED);
}
TEST_F(IntegrationSession, ExactPrefixMissAndRecomputeWireRanges) {
  Publish();
  auto exact = Call(s, Lookup());
  ASSERT_EQ(exact.status(), v1::OK);
  EXPECT_EQ(exact.hit_tokens(), 4U);
  EXPECT_FALSE(exact.recompute());
  EXPECT_EQ(exact.recompute_ranges_size(), 0);
  auto prefix = Call(s, Lookup(8, false));
  ASSERT_EQ(prefix.status(), v1::OK) << prefix.message();
  EXPECT_EQ(prefix.hit_tokens(), 4U);
  ASSERT_EQ(prefix.hit_ranges_size(), 1);
  EXPECT_EQ(prefix.hit_ranges(0).begin(), 0U);
  EXPECT_EQ(prefix.hit_ranges(0).end(), 4U);
  ASSERT_EQ(prefix.recompute_ranges_size(), 1);
  EXPECT_EQ(prefix.recompute_ranges(0).begin(), 4U);
  EXPECT_EQ(prefix.recompute_ranges(0).end(), 8U);
  EXPECT_TRUE(prefix.recompute());
  EXPECT_EQ(prefix.manifest().token_count(), 4U);
  EXPECT_EQ(prefix.layer_count(), 1U);
  ASSERT_EQ(prefix.chunk_indices_size(), 1);
  EXPECT_EQ(prefix.chunk_indices(0), 0U);
  auto miss = Call(s, Lookup(8));
  EXPECT_EQ(miss.status(), v1::NOT_FOUND);
  EXPECT_EQ(miss.lease_id(), 0U);
  ASSERT_EQ(miss.recompute_ranges_size(), 1);
  EXPECT_EQ(miss.recompute_ranges(0).begin(), 0U);
  EXPECT_EQ(miss.recompute_ranges(0).end(), 8U);
  auto incompatible = Lookup();
  incompatible.mutable_manifest()->set_adapter_revision("other");
  EXPECT_EQ(Call(s, incompatible).status(), v1::NOT_FOUND);
}
TEST_F(IntegrationSession, UnknownPayloadDigestLookupReturnsStoredDigestAndRanges) {
  Publish();
  const auto stored = ToProto(Manifest());
  for (const std::size_t size : {0U, 32U}) {
    SCOPED_TRACE(size);
    for (const bool exact : {true, false}) {
      SCOPED_TRACE(exact);
      auto request = Lookup(exact ? 4U : 8U, exact);
      request.mutable_manifest()->set_payload_digest(std::string(size, '\0'));
      const auto reply = Call(s, request);
      ASSERT_EQ(reply.status(), v1::OK) << reply.message();
      ASSERT_NE(reply.lease_id(), 0U);
      EXPECT_EQ(reply.manifest().SerializeAsString(), stored.SerializeAsString());
      EXPECT_EQ(reply.manifest().payload_digest(), stored.payload_digest());
      EXPECT_EQ(reply.hit_tokens(), 4U);
      ASSERT_EQ(reply.hit_ranges_size(), 1);
      EXPECT_EQ(reply.hit_ranges(0).begin(), 0U);
      EXPECT_EQ(reply.hit_ranges(0).end(), 4U);
      EXPECT_EQ(reply.recompute(), !exact);
      ASSERT_EQ(reply.recompute_ranges_size(), exact ? 0 : 1);
      if (!exact) {
        EXPECT_EQ(reply.recompute_ranges(0).begin(), 4U);
        EXPECT_EQ(reply.recompute_ranges(0).end(), 8U);
      }
      EXPECT_EQ(Op(s, v1::RELEASE, reply.lease_id()).status(), v1::OK);

      request.mutable_manifest()->set_adapter_revision("not-cached");
      const auto miss = Call(s, request);
      EXPECT_EQ(miss.status(), v1::NOT_FOUND);
      EXPECT_FALSE(miss.has_manifest());
      EXPECT_EQ(miss.lease_id(), 0U);
      EXPECT_EQ(miss.hit_tokens(), 0U);
      EXPECT_EQ(miss.hit_ranges_size(), 0);
      EXPECT_TRUE(miss.recompute());
      ASSERT_EQ(miss.recompute_ranges_size(), 1);
      EXPECT_EQ(miss.recompute_ranges(0).begin(), 0U);
      EXPECT_EQ(miss.recompute_ranges(0).end(), exact ? 4U : 8U);
    }
  }
}
TEST_F(IntegrationSession, ReserveRejectsUnknownPayloadDigestWithoutAllocating) {
  for (const std::size_t size : {0U, 32U}) {
    SCOPED_TRACE(size);
    auto request = Request(v1::RESERVE);
    *request.mutable_manifest() = ToProto(Manifest());
    request.mutable_manifest()->set_payload_digest(std::string(size, '\0'));
    EXPECT_EQ(Call(s, request).status(), v1::INVALID_ARGUMENT);
    EXPECT_EQ(registry.Pending(), 0U);
    EXPECT_EQ(registry.TrackedBytes(), 0U);
    EXPECT_EQ(index.Size(), 0U);
  }
  Publish();
  EXPECT_EQ(registry.Size(), 1U);
}
TEST_F(IntegrationSession, QueryAndPublicationRejectMalformedDigestLengths) {
  for (const auto operation : {v1::LOOKUP, v1::RESERVE}) {
    SCOPED_TRACE(operation);
    for (const std::size_t size : {1U, 31U, 33U}) {
      SCOPED_TRACE(size);
      auto request = Lookup();
      request.set_operation(operation);
      request.mutable_manifest()->set_payload_digest(std::string(size, 'x'));
      EXPECT_EQ(Call(s, request).status(), v1::INVALID_ARGUMENT);
    }
  }
  EXPECT_EQ(registry.Pending(), 0U);
  EXPECT_EQ(registry.TrackedBytes(), 0U);
}
TEST_F(IntegrationSession, UnknownPayloadDigestDoesNotRelaxQueryIdentityValidation) {
  Publish();
  auto request = Lookup();
  request.mutable_manifest()->clear_payload_digest();
  request.mutable_manifest()->clear_token_digest();
  EXPECT_EQ(Call(s, request).status(), v1::INVALID_ARGUMENT);
  request.mutable_manifest()->set_token_digest(std::string(32, '\0'));
  EXPECT_EQ(Call(s, request).status(), v1::INVALID_ARGUMENT);
  request = Lookup();
  request.mutable_manifest()->clear_payload_digest();
  request.mutable_manifest()->set_payload_bytes(0);
  EXPECT_EQ(Call(s, request).status(), v1::INVALID_ARGUMENT);
  request = Lookup();
  request.mutable_manifest()->clear_payload_digest();
  request.mutable_manifest()->set_tenant_id("foreign");
  EXPECT_EQ(Call(s, request).status(), v1::INVALID_ARGUMENT);
}
TEST_F(IntegrationSession, DeadlineCancellationAndChecksumErrorsDoNotMutate) {
  auto id = Reserve(s);
  auto r = Request(v1::COMMIT);
  r.set_reservation_id(id);
  r.set_deadline_unix_ms(1);
  EXPECT_EQ(Call(s, r).status(), v1::DEADLINE_EXCEEDED);
  EXPECT_EQ(registry.Pending(), 1U);
  r.set_deadline_unix_ms(0);
  r.set_cancelled(true);
  EXPECT_EQ(Call(s, r).status(), v1::CANCELLED);
  EXPECT_EQ(registry.Pending(), 1U);
  r.set_operation(v1::PUT);
  r.set_cancelled(false);
  r.set_payload(std::string(128, 'x'));
  EXPECT_EQ(Call(s, r).status(), v1::CORRUPTION);
  EXPECT_EQ(Put(s, id).status(), v1::OK);
  EXPECT_EQ(Put(s, id).status(), v1::ALREADY_EXISTS);
  EXPECT_EQ(Op(s, v1::COMMIT, id).status(), v1::OK);
  auto lookup = Lookup(8, false);
  lookup.set_cancelled(true);
  auto result = Call(s, lookup);
  EXPECT_EQ(result.status(), v1::CANCELLED);
  ASSERT_EQ(result.recompute_ranges_size(), 1);
  EXPECT_EQ(result.recompute_ranges(0).end(), 8U);
}
TEST_F(IntegrationSession, LeaseBudgetReturnsRecomputeAndRecovers) {
  auto first = Publish();
  for (int i = 0; i < 63; ++i) ASSERT_EQ(Call(s, Lookup()).status(), v1::OK);
  auto full = Call(s, Lookup());
  EXPECT_EQ(full.status(), v1::LIMIT_EXCEEDED);
  EXPECT_TRUE(full.recompute());
  EXPECT_EQ(full.lease_id(), 0U);
  EXPECT_EQ(Op(s, v1::RELEASE, first).status(), v1::OK);
  EXPECT_EQ(Call(s, Lookup()).status(), v1::OK);
}
TEST_F(IntegrationSession, IndexCapacityFailurePreservesReservation) {
  kvcache::MatchIndex small({1, 4096, 131072, 4096, 16});
  auto other = Manifest();
  other.adapter_revision = "other";
  ASSERT_TRUE(small.Insert(other, kvcache::CanonicalCacheKey(other).value()).ok());
  Session target(registry, small, "tenant", "m");
  Negotiate(target);
  auto id = Reserve(target);
  EXPECT_EQ(Put(target, id).status(), v1::OK);
  EXPECT_EQ(Op(target, v1::COMMIT, id).status(), v1::LIMIT_EXCEEDED);
  EXPECT_EQ(registry.Pending(), 1U);
  EXPECT_EQ(registry.Size(), 0U);
  ASSERT_TRUE(small.Erase(other).ok());
  EXPECT_EQ(Op(target, v1::COMMIT, id).status(), v1::OK);
}
TEST_F(IntegrationSession, ConcurrentLookupReleaseAndForeignAttempts) {
  auto lease = Publish();
  std::vector<std::future<void>> workers;
  for (int i = 0; i < 4; ++i)
    workers.push_back(std::async(std::launch::async, [&] {
      Session client(registry, index, "tenant", "m");
      Negotiate(client);
      for (int j = 0; j < 20; ++j) {
        EXPECT_EQ(Op(client, v1::RELEASE, lease).status(), v1::NOT_FOUND);
        auto result = Call(client, Lookup());
        EXPECT_EQ(result.status(), v1::OK);
        EXPECT_EQ(Op(client, v1::GET, result.lease_id()).status(), v1::OK);
        EXPECT_EQ(Op(client, v1::RELEASE, result.lease_id()).status(), v1::OK);
      }
    }));
  for (auto& worker : workers) {
    EXPECT_EQ(worker.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    worker.get();
  }
}
TEST_F(IntegrationSession, MultiChunkOutOfOrderAndInvalidChunk) {
  auto id = Reserve(s, Manifest(8));
  EXPECT_EQ(Put(s, id, 'x', 2).status(), v1::INVALID_ARGUMENT);
  EXPECT_EQ(Put(s, id, 'x', 1).status(), v1::OK);
  EXPECT_EQ(Op(s, v1::COMMIT, id).status(), v1::BUSY);
  EXPECT_EQ(Put(s, id, 'x', 0).status(), v1::OK);
  auto commit = Op(s, v1::COMMIT, id);
  ASSERT_EQ(commit.status(), v1::OK);
  auto request = Request(v1::GET);
  request.set_lease_id(commit.lease_id());
  request.set_chunk_index(1);
  auto reply = Call(s, request);
  EXPECT_EQ(reply.status(), v1::OK);
  EXPECT_EQ(reply.payload(), std::string(128, 'x'));
  EXPECT_EQ(reply.payload_checksum(), Crc32(Bytes(128, std::byte{'x'})));
  request.set_chunk_index(2);
  EXPECT_EQ(Call(s, request).status(), v1::INVALID_ARGUMENT);
}
TEST_F(IntegrationSession, RegistryCommitCapacityFailureRetainsAndCleansReservation) {
  kvcache::RegistryLimits limits;
  limits.max_objects = 1;
  kvcache::ChunkRegistry limited(limits);
  kvcache::MatchIndex matches;
  {
    Session target(limited, matches, "tenant", "m");
    Negotiate(target);
    auto first = Reserve(target);
    auto other = Manifest();
    other.adapter_revision = "other";
    auto second = Reserve(target, other);
    EXPECT_EQ(Put(target, first).status(), v1::OK);
    EXPECT_EQ(Put(target, second).status(), v1::OK);
    EXPECT_EQ(Op(target, v1::COMMIT, first).status(), v1::OK);
    EXPECT_EQ(Op(target, v1::COMMIT, second).status(), v1::LIMIT_EXCEEDED);
    EXPECT_EQ(limited.Pending(), 1U);
    EXPECT_EQ(matches.Size(), 1U);
  }
  EXPECT_EQ(limited.Pending(), 0U);
}
TEST_F(IntegrationSession, SharedSessionConcurrentReleaseHasOneWinner) {
  auto lease = Publish();
  auto first = std::async(std::launch::async, [&] { return Op(s, v1::RELEASE, lease).status(); });
  auto second = std::async(std::launch::async, [&] { return Op(s, v1::RELEASE, lease).status(); });
  ASSERT_EQ(first.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  ASSERT_EQ(second.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  const auto a = first.get(), b = second.get();
  EXPECT_TRUE((a == v1::OK && b == v1::NOT_FOUND) || (a == v1::NOT_FOUND && b == v1::OK));
  EXPECT_EQ(registry.Size(), 1U);
}
TEST(IntegrationCodec, PipelineObjectCountBound) {
  auto bytes = Codec::Encode(Frame{1, kMajor, kMinor, {}});
  ASSERT_TRUE(bytes.ok());
  Bytes input;
  for (int i = 0; i < 4097; ++i)
    input.insert(input.end(), bytes.value().begin(), bytes.value().end());
  Bytes buffer;
  EXPECT_EQ(Codec::Feed(buffer, input).status().code(), StatusCode::kLimitExceeded);
}
}  // namespace
}  // namespace kvstore::integration
