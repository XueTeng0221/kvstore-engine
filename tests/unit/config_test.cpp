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

}  // namespace kvstore
