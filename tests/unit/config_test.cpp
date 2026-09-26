#include "kvstore/config/config.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

namespace kvstore {
namespace {

std::filesystem::path DefaultConfigPath() {
  return std::filesystem::path(KVSTORE_SOURCE_DIR) / "config/config.json";
}

nlohmann::json DefaultJson() {
  std::ifstream input(DefaultConfigPath());
  return nlohmann::json::parse(input);
}

}  // namespace

TEST(ConfigTest, LoadsDefaultConfiguration) {
  const auto result = Config::Load(DefaultConfigPath());
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result.value().engine.type, "hash");
  EXPECT_EQ(result.value().replication.backlog_slots, 1024U);
}

TEST(ConfigTest, RejectsInvalidKvCachePolicySchedulerLimits) {
  auto json = DefaultJson();
  json["kvcache"]["admission_policy"] = "unknown";
  auto result = Config::Parse(json.dump(), DefaultConfigPath().parent_path());
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.status().message().find("$.kvcache"), std::string_view::npos);

  for (const auto* field : {"max_pending_loads", "max_inflight_io_bytes", "max_policy_scan",
                            "tenant_quantum", "max_tracked_objects"}) {
    json = DefaultJson();
    json["kvcache"][field] = 0;
    result = Config::Parse(json.dump(), DefaultConfigPath().parent_path());
    EXPECT_FALSE(result.ok()) << field;
  }
  json = DefaultJson();
  json["kvcache"]["low_reuse_weight"] = -1.0;
  EXPECT_FALSE(Config::Parse(json.dump(), DefaultConfigPath().parent_path()).ok());
}

TEST(ConfigTest, RejectsUnknownField) {
  const auto result = Config::Parse(R"({"server":{},"unexpected":1})");
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::kInvalidArgument);
  EXPECT_NE(result.status().message().find("unexpected"), std::string_view::npos);
}

TEST(ConfigTest, RejectsReplicaWithoutUpstream) {
  auto json = DefaultJson();
  json["replication"]["role"] = "replica";
  const auto result = Config::Parse(json.dump(), DefaultConfigPath().parent_path());
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.status().message().find("upstream"), std::string_view::npos);
}

TEST(ConfigTest, RejectsMalformedReplicationUpstream) {
  auto json = DefaultJson();
  json["replication"]["role"] = "replica";
  for (const auto* upstream : {"localhost:6380", "127.0.0.1", "127.0.0.1:0", "127.0.0.1:abc"}) {
    json["replication"]["upstream"] = upstream;
    const auto result = Config::Parse(json.dump(), DefaultConfigPath().parent_path());
    EXPECT_FALSE(result.ok()) << upstream;
    EXPECT_NE(result.status().message().find("$.replication.upstream"), std::string_view::npos);
  }
}

TEST(ConfigTest, BoundsReplicationHeartbeatIntervalBeforeDurationConversion) {
  auto json = DefaultJson();
  const auto max_safe = std::numeric_limits<std::int64_t>::max() / 3;
  json["replication"]["heartbeat_interval_ms"] = max_safe;
  EXPECT_TRUE(Config::Parse(json.dump(), DefaultConfigPath().parent_path()).ok());
  json["replication"]["heartbeat_interval_ms"] = max_safe + 1;
  EXPECT_FALSE(Config::Parse(json.dump(), DefaultConfigPath().parent_path()).ok());
}

TEST(ConfigTest, AcceptsImplementedReplicationExecutorsAndRejectsUnknown) {
  auto json = DefaultJson();
  for (const auto* backend : {"pthread", "reactor", "proactor", "io_uring", "ntyco"}) {
    json["replication"]["backend"] = backend;
    const auto result = Config::Parse(json.dump(), DefaultConfigPath().parent_path());
    EXPECT_TRUE(result.ok()) << backend;
  }
  json["replication"]["backend"] = "unknown";
  const auto result = Config::Parse(json.dump(), DefaultConfigPath().parent_path());
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.status().message().find("$.replication.backend"), std::string_view::npos);
}

TEST(ConfigTest, AcceptsNtycoNetworkBackend) {
  auto json = DefaultJson();
  json["network"]["backend"] = "ntyco";
  const auto result = Config::Parse(json.dump(), DefaultConfigPath().parent_path());
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result.value().network.backend, "ntyco");
}

TEST(ConfigTest, AcceptsIoUringNetworkSelection) {
  auto json = DefaultJson();
  json["network"]["backend"] = "io_uring";
  const auto result = Config::Parse(json.dump(), DefaultConfigPath().parent_path());
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result.value().network.backend, "io_uring");
}

TEST(ConfigTest, RejectsIoUringQueueDepthAboveRuntimeLimit) {
  auto json = DefaultJson();
  json["network"]["io_uring"]["queue_depth"] = 4097;
  EXPECT_FALSE(Config::Parse(json.dump(), DefaultConfigPath().parent_path()).ok());
}

TEST(ConfigTest, RejectsOutputBufferTooSmallForReplicationFrameReserve) {
  auto json = DefaultJson();
  json["protocol"]["max_key_bytes"] = 1;
  json["protocol"]["max_value_bytes"] = 1;
  json["protocol"]["max_frame_bytes"] = 1024;
  json["server"]["max_output_buffer_bytes"] = 1062;
  json["server"]["output_high_watermark_bytes"] = 1000;
  const auto rejected = Config::Parse(json.dump(), DefaultConfigPath().parent_path());
  ASSERT_FALSE(rejected.ok());
  EXPECT_NE(rejected.status().message().find("replication chunk"), std::string_view::npos);
  json["server"]["max_output_buffer_bytes"] = 1063;
  json["server"]["output_high_watermark_bytes"] = 1000;
  EXPECT_TRUE(Config::Parse(json.dump(), DefaultConfigPath().parent_path()).ok());
}

TEST(ConfigTest, RejectsWrongTypeAndConflictingFrameLimit) {
  auto json = DefaultJson();
  json["server"]["port"] = "6380";
  auto result = Config::Parse(json.dump(), DefaultConfigPath().parent_path());
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.status().message().find("$.server.port"), std::string_view::npos);

  json = DefaultJson();
  json["protocol"]["max_frame_bytes"] = 1;
  result = Config::Parse(json.dump(), DefaultConfigPath().parent_path());
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.status().message().find("max_frame_bytes"), std::string_view::npos);
}

TEST(ConfigTest, RejectsNegativeUnsignedAndMissingNestedObjectWithPath) {
  auto json = DefaultJson();
  json["engine"]["capacity"] = -1;
  auto result = Config::Parse(json.dump(), DefaultConfigPath().parent_path());
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.status().message().find("$.engine.capacity"), std::string_view::npos);

  json = DefaultJson();
  json["network"].erase("io_uring");
  result = Config::Parse(json.dump(), DefaultConfigPath().parent_path());
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.status().message().find("$.network.io_uring"), std::string_view::npos);
}

TEST(ConfigTest, RejectsFrameSizeOverflow) {
  auto json = DefaultJson();
  json["protocol"]["max_key_bytes"] = 1;
  json["protocol"]["max_value_bytes"] = std::numeric_limits<std::uint64_t>::max();
  json["protocol"]["max_frame_bytes"] = 128;
  const auto result = Config::Parse(json.dump(), DefaultConfigPath().parent_path());
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.status().message().find("max_frame_bytes"), std::string_view::npos);
}

TEST(ConfigTest, RejectsZeroEnabledIntervalsAndEmptyMetricsAddress) {
  auto json = DefaultJson();
  json["persistence"]["snapshot_interval_s"] = 0;
  EXPECT_FALSE(Config::Parse(json.dump(), DefaultConfigPath().parent_path()).ok());

  json = DefaultJson();
  json["replication"]["handshake_timeout_ms"] = 0;
  EXPECT_FALSE(Config::Parse(json.dump(), DefaultConfigPath().parent_path()).ok());

  json = DefaultJson();
  json["replication"]["heartbeat_interval_ms"] = 0;
  EXPECT_FALSE(Config::Parse(json.dump(), DefaultConfigPath().parent_path()).ok());

  json = DefaultJson();
  json["kvcache"]["load_timeout_ms"] = 0;
  EXPECT_FALSE(Config::Parse(json.dump(), DefaultConfigPath().parent_path()).ok());

  json = DefaultJson();
  json["observability"]["metrics_address"] = "";
  EXPECT_FALSE(Config::Parse(json.dump(), DefaultConfigPath().parent_path()).ok());
}

TEST(ConfigTest, ReportsMissingUpstreamWithJsonPath) {
  auto json = DefaultJson();
  json["replication"].erase("upstream");
  const auto result = Config::Parse(json.dump(), DefaultConfigPath().parent_path());
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.status().message().find("$.replication.upstream"), std::string_view::npos);
}

TEST(ConfigTest, RedactedOutputIsCompleteAndDoesNotInventSecrets) {
  const auto result = Config::Load(DefaultConfigPath());
  ASSERT_TRUE(result.ok());
  const auto output = result.value().ToRedactedJson();
  EXPECT_NE(output.find("max_inflight_requests"), std::string::npos);
  EXPECT_NE(output.find("max_concurrent_loads"), std::string::npos);
  EXPECT_EQ(output.find("credentials"), std::string::npos);
}

TEST(ConfigTest, RejectsUnimplementedRuntimeOptionsAndUnsafeTimeouts) {
  auto json = DefaultJson();
  json["persistence"]["mmap_load"] = false;
  EXPECT_FALSE(Config::Parse(json.dump(), DefaultConfigPath().parent_path()).ok());

  json = DefaultJson();
  json["persistence"]["io_uring_write"] = true;
  json["persistence"]["allow_sync_fallback"] = false;
  EXPECT_FALSE(Config::Parse(json.dump(), DefaultConfigPath().parent_path()).ok());

  json["persistence"]["allow_sync_fallback"] = true;
  EXPECT_TRUE(Config::Parse(json.dump(), DefaultConfigPath().parent_path()).ok());

  json = DefaultJson();
  json["protocol"]["parse_timeout_ms"] = std::numeric_limits<std::uint64_t>::max();
  EXPECT_FALSE(Config::Parse(json.dump(), DefaultConfigPath().parent_path()).ok());
}

}  // namespace kvstore
