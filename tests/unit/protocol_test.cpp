#include <gtest/gtest.h>

#include <limits>
#include <stop_token>
#include <thread>

#include "kvstore/command/dispatcher.hpp"
#include "kvstore/engine/hash_engine.hpp"
#include "kvstore/protocol/batch.hpp"
#include "kvstore/protocol/native.hpp"
#include "kvstore/protocol/resp.hpp"

namespace kvstore {

TEST(ProtocolTest, RespHandlesPipeliningAndFragmentation) {
  RespParser parser(1024);
  ASSERT_TRUE(parser.Feed("*2\r\n$4\r\nPING\r\n$4\r\npo").ok());
  auto first = parser.ParseAvailable();
  ASSERT_TRUE(first.ok());
  EXPECT_TRUE(first.value().empty());
  ASSERT_TRUE(parser.Feed("ng\r\n*2\r\n$4\r\nECHO\r\n$2\r\nhi\r\n").ok());
  auto commands = parser.ParseAvailable();
  ASSERT_TRUE(commands.ok()) << commands.status().message();
  ASSERT_EQ(commands.value().size(), 2U);
  EXPECT_EQ(commands.value()[0].type, CommandType::kPing);
  EXPECT_EQ(commands.value()[1].type, CommandType::kEcho);
}

TEST(ProtocolTest, RespPreservesBinaryBulkAndRejectsMalformedFrames) {
  RespParser parser(1024);
  const std::string resp_frame = "*2\r\n$3\r\nGET\r\n$3\r\n" + std::string("a\0b", 3) + "\r\n";
  ASSERT_TRUE(parser.Feed(resp_frame).ok());
  auto commands = parser.ParseAvailable();
  ASSERT_TRUE(commands.ok());
  ASSERT_EQ(commands.value().size(), 1U);
  ASSERT_EQ(commands.value()[0].args[0].size(), 3U);
  EXPECT_EQ(commands.value()[0].args[0][1], '\0');

  RespParser malformed(128);
  ASSERT_TRUE(malformed.Feed("*1\r\n:4\r\nPING\r\n").ok());
  EXPECT_FALSE(malformed.ParseAvailable().ok());
}

TEST(ProtocolTest, NativeHandlesBinaryPayloadAndPipelining) {
  NativeParser parser(64, 64, 256);
  const std::string native_frame = "KV/1 SET 3 3\r\n" + std::string("a\0b", 3) +
                                   std::string("xy\0", 3) + "KV/1 GET 3 0\r\n" +
                                   std::string("a\0b", 3);
  ASSERT_TRUE(parser.Feed(native_frame).ok());
  auto commands = parser.ParseAvailable();
  ASSERT_TRUE(commands.ok()) << commands.status().message();
  ASSERT_EQ(commands.value().size(), 2U);
  EXPECT_EQ(commands.value()[0].args[0].size(), 3U);
  EXPECT_EQ(commands.value()[0].args[1][2], '\0');
}

TEST(ProtocolTest, ResponsesEncodeRespNullAndErrors) {
  EXPECT_EQ(EncodeResp({true, {}, {}, false, true, {}}), "$-1\r\n");
  EXPECT_EQ(EncodeResp({true, "PONG", {}, false, false, {}, true}), "+PONG\r\n");
  EXPECT_EQ(EncodeResp({false, {}, "bad", false, false, {}}), "-ERR bad\r\n");
}

TEST(ProtocolTest, DispatcherUsesRedisOverwriteAndTypedResponses) {
  Dispatcher dispatcher(std::make_unique<HashEngine>(16));
  auto response =
      dispatcher.Execute({CommandType::kSet, {"key", "v1"}, WriteSource::kClient, true});
  ASSERT_TRUE(response.ok);
  response = dispatcher.Execute({CommandType::kSet, {"key", "v2"}, WriteSource::kClient, true});
  ASSERT_TRUE(response.ok);
  EXPECT_EQ(dispatcher.Execute({CommandType::kGet, {"key"}, WriteSource::kClient, true}).value,
            "v2");
  EXPECT_TRUE(
      dispatcher.Execute({CommandType::kGet, {"missing"}, WriteSource::kClient, true}).null_value);
  EXPECT_EQ(dispatcher.Execute({CommandType::kDel, {"missing"}, WriteSource::kClient, true}).value,
            "0");
  ASSERT_TRUE(
      dispatcher.Execute({CommandType::kSet, {"typed", "OK"}, WriteSource::kClient, true}).ok);
  EXPECT_EQ(
      EncodeResp(dispatcher.Execute({CommandType::kGet, {"typed"}, WriteSource::kClient, true})),
      "$2\r\nOK\r\n");
}

TEST(ProtocolTest, ReplicaDispatcherRejectsWritesButServesReads) {
  Dispatcher dispatcher(std::make_unique<HashEngine>(16));
  dispatcher.SetReadOnly(true);
  const auto write = dispatcher.Execute({CommandType::kSet, {"key", "value"}});
  EXPECT_FALSE(write.ok);
  EXPECT_EQ(write.error, "READ_ONLY replica is read-only");
  const auto read = dispatcher.Execute({CommandType::kGet, {"key"}});
  EXPECT_FALSE(read.ok);
  EXPECT_EQ(read.error, "NOT_FOUND key not found");
}

TEST(ProtocolTest, DispatcherRollsBackWhenEventSinkFails) {
  Dispatcher dispatcher(std::make_unique<HashEngine>(16), [](const WriteEvent&) {
    return Status{StatusCode::kIoError, "sink failed"};
  });
  const auto response = dispatcher.Execute({CommandType::kSet, {"key", "value"}});
  EXPECT_FALSE(response.ok);
  EXPECT_FALSE(dispatcher.Execute({CommandType::kGet, {"key"}}).ok);
  EXPECT_TRUE(
      dispatcher.Execute({CommandType::kGet, {"key"}, WriteSource::kClient, true}).null_value);
  EXPECT_EQ(dispatcher.next_offset(), 1U);
}

TEST(ProtocolTest, DispatcherAdvancesSequenceWithoutPersistenceSink) {
  Dispatcher dispatcher(std::make_unique<HashEngine>(16));
  ASSERT_TRUE(dispatcher.Execute({CommandType::kSet, {"key", "value"}}).ok);
  EXPECT_EQ(dispatcher.next_offset(), 2U);
  EXPECT_EQ(dispatcher.next_event_id(), 2U);
}

TEST(ProtocolTest, WriteEventCarriesOriginTimestampAndChecksum) {
  std::vector<WriteEvent> events;
  Dispatcher dispatcher(
      std::make_unique<HashEngine>(16),
      [&events](const WriteEvent& event) {
        events.push_back(event);
        return Status::Ok();
      },
      {}, {}, 7, 11, "node-test");
  ASSERT_TRUE(dispatcher.Execute({CommandType::kSet, {"key", "value"}}).ok);
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].offset, 7U);
  EXPECT_EQ(events[0].event_id, 11U);
  EXPECT_EQ(events[0].origin_node, "node-test");
  EXPECT_GT(events[0].timestamp_ms, 0U);
  EXPECT_NE(events[0].checksum, 0U);
  EXPECT_EQ(events[0].checksum, events[0].ComputeChecksum());
}

TEST(ProtocolTest, BatchParsesMixedRecordsAndRejectsTrailingBytes) {
  std::string payload;
  payload.push_back(static_cast<char>(CommandType::kSet));
  payload.append("\0\0\0\1", 4);
  payload.append("\0\0\0\0\0\0\0\1", 8);
  payload.push_back('k');
  payload.push_back('v');
  payload.push_back(static_cast<char>(CommandType::kGet));
  payload.append("\0\0\0\1", 4);
  payload.append("\0\0\0\0\0\0\0\0", 8);
  payload.push_back('k');
  std::string frame("KVB1\0\1\0\0\0\0\0\2", 12);
  frame.append("\0\0\0\0\0\0\0\x1d", 8);
  frame += payload;
  BatchParser parser(4, 256);
  ASSERT_TRUE(parser.Feed(frame).ok());
  const auto commands = parser.ParseAvailable();
  ASSERT_TRUE(commands.ok());
  ASSERT_EQ(commands.value().size(), 2U);
  EXPECT_EQ(commands.value()[0].type, CommandType::kSet);
  EXPECT_EQ(commands.value()[1].type, CommandType::kGet);
}

TEST(ProtocolTest, DispatcherCoversNativeCommandsAndRequestContext) {
  bool saved = false;
  bool loaded = false;
  Dispatcher dispatcher(
      std::make_unique<HashEngine>(16), {},
      [&saved] {
        saved = true;
        return Status::Ok();
      },
      [&loaded] {
        loaded = true;
        return Status::Ok();
      });
  EXPECT_TRUE(dispatcher.Execute({CommandType::kSet, {"key", "one"}}).ok);
  EXPECT_FALSE(dispatcher.Execute({CommandType::kSet, {"key", "two"}}).ok);
  EXPECT_TRUE(dispatcher.Execute({CommandType::kMod, {"key", "two"}}).ok);
  EXPECT_EQ(dispatcher.Execute({CommandType::kExist, {"key"}}).value, "1");
  EXPECT_TRUE(dispatcher.Execute({CommandType::kSave, {}}).ok);
  EXPECT_TRUE(dispatcher.Execute({CommandType::kLoad, {}}).ok);
  EXPECT_TRUE(saved);
  EXPECT_TRUE(loaded);

  std::stop_source stop;
  stop.request_stop();
  RequestContext cancelled;
  cancelled.stop = stop.get_token();
  EXPECT_FALSE(dispatcher.Execute({CommandType::kGet, {"key"}}, cancelled).ok);
  RequestContext expired;
  expired.deadline = std::chrono::steady_clock::now();
  EXPECT_FALSE(dispatcher.Execute({CommandType::kGet, {"key"}}, expired).ok);
}

TEST(ProtocolTest, RedisMultiKeyCountersMgetAndArityAreCompatible) {
  Dispatcher dispatcher(std::make_unique<HashEngine>(16));
  ASSERT_TRUE(dispatcher.Execute({CommandType::kSet, {"a", "1"}, WriteSource::kClient, true}).ok);
  ASSERT_TRUE(dispatcher.Execute({CommandType::kSet, {"b", "2"}, WriteSource::kClient, true}).ok);
  EXPECT_EQ(
      dispatcher.Execute({CommandType::kExist, {"a", "x", "b"}, WriteSource::kClient, true}).value,
      "2");
  const auto mget =
      dispatcher.Execute({CommandType::kMget, {"b", "missing", "a"}, WriteSource::kClient, true});
  ASSERT_EQ(mget.array.size(), 3U);
  EXPECT_EQ(mget.array[0].value, "2");
  EXPECT_TRUE(mget.array[1].null_value);
  EXPECT_EQ(mget.array[2].value, "1");
  EXPECT_EQ(
      dispatcher.Execute({CommandType::kDel, {"a", "missing", "b"}, WriteSource::kClient, true})
          .value,
      "2");
  EXPECT_FALSE(
      dispatcher.Execute({CommandType::kIncr, {"counter", "2"}, WriteSource::kClient, true}).ok);
  EXPECT_EQ(dispatcher.Execute({CommandType::kIncrBy, {"counter", "2"}, WriteSource::kClient, true})
                .value,
            "2");
}

TEST(ProtocolTest, RedisMultiDeletePublishesOneLogicalEvent) {
  std::vector<WriteEvent> events;
  Dispatcher dispatcher(std::make_unique<HashEngine>(16), [&events](const WriteEvent& event) {
    events.push_back(event);
    return Status::Ok();
  });
  ASSERT_TRUE(dispatcher.Execute({CommandType::kSet, {"a", "1"}, WriteSource::kClient, true}).ok);
  ASSERT_TRUE(dispatcher.Execute({CommandType::kSet, {"b", "2"}, WriteSource::kClient, true}).ok);
  events.clear();
  const auto response =
      dispatcher.Execute({CommandType::kDel, {"a", "missing", "b"}, WriteSource::kClient, true});
  EXPECT_TRUE(response.ok);
  EXPECT_EQ(response.value, "2");
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, CommandType::kDelMany);
  EXPECT_EQ(dispatcher.next_offset(), 4U);
}

TEST(ProtocolTest, UnknownRespCommandAndResp3NegotiationAreTypedErrors) {
  auto unknown = CommandFromResp({"NOT-A-COMMAND"});
  ASSERT_TRUE(unknown.ok());
  EXPECT_EQ(unknown.value().type, CommandType::kUnknown);
  auto hello = CommandFromResp({"HELLO", "3"});
  ASSERT_TRUE(hello.ok());
  EXPECT_EQ(hello.value().type, CommandType::kUnknown);
  Dispatcher dispatcher(std::make_unique<HashEngine>(16));
  EXPECT_FALSE(dispatcher.Execute(unknown.value()).ok);
}

TEST(ProtocolTest, NativeManagementCommandsUseEmptyPayload) {
  NativeParser parser(64, 64, 256);
  ASSERT_TRUE(parser.Feed("KV/1 SAVE 0 0\r\nKV/1 LOAD 0 0\r\n").ok());
  const auto commands = parser.ParseAvailable();
  ASSERT_TRUE(commands.ok()) << commands.status().message();
  ASSERT_EQ(commands.value().size(), 2U);
  EXPECT_EQ(commands.value()[0].type, CommandType::kSave);
  EXPECT_EQ(commands.value()[1].type, CommandType::kLoad);
  EXPECT_TRUE(commands.value()[0].args.empty());
}

TEST(ProtocolTest, NativeAndRespEnforceIncrementalBufferLimits) {
  NativeParser native(8, 8, 32);
  ASSERT_TRUE(native.Feed("KV/1 SET 1 2\r\nk").ok());
  EXPECT_TRUE(native.ParseAvailable().value().empty());
  ASSERT_TRUE(native.Feed("v1").ok());
  EXPECT_EQ(native.ParseAvailable().value().size(), 1U);
  EXPECT_EQ(native.Feed(std::string(33, 'x')).code(), StatusCode::kLimitExceeded);

  RespParser resp(32);
  EXPECT_EQ(resp.Feed(std::string(33, 'x')).code(), StatusCode::kLimitExceeded);
  ASSERT_TRUE(resp.Feed("*1\r\n$999999999999999999999\r\n").ok());
  EXPECT_FALSE(resp.ParseAvailable().ok());
}

TEST(ProtocolTest, BatchRejectsMalformedHeaderAndPreservesFrameBoundaries) {
  BatchParser parser(2, 64, 128);
  const std::string malformed("BAD!\0\1\0\0\0\0\0\1\0\0\0\0\0\0\0\0", 20);
  ASSERT_TRUE(parser.Feed(malformed).ok());
  EXPECT_FALSE(parser.ParseAvailableFrames().ok());

  BatchParser limited(1, 32, 32);
  EXPECT_EQ(limited.Feed(std::string(33, 'x')).code(), StatusCode::kLimitExceeded);
}

TEST(ProtocolTest, RedisOverwriteAndIncrementRemainLinearAcrossThreads) {
  Dispatcher dispatcher(std::make_unique<HashEngine>(64));
  ASSERT_TRUE(
      dispatcher.Execute({CommandType::kSet, {"shared", "0"}, WriteSource::kClient, true}).ok);
  std::vector<std::jthread> writers;
  for (int thread = 0; thread < 4; ++thread) {
    writers.emplace_back([&dispatcher, thread] {
      for (int iteration = 0; iteration < 100; ++iteration) {
        const auto response =
            dispatcher.Execute({CommandType::kSet,
                                {"shared", std::to_string(thread * 100 + iteration)},
                                WriteSource::kClient,
                                true});
        EXPECT_TRUE(response.ok);
      }
    });
  }
  writers.clear();
  EXPECT_EQ(dispatcher.next_offset(), 402U);

  ASSERT_TRUE(dispatcher
                  .Execute({CommandType::kSet,
                            {"counter", std::to_string(std::numeric_limits<std::int64_t>::max())},
                            WriteSource::kClient,
                            true})
                  .ok);
  EXPECT_FALSE(
      dispatcher.Execute({CommandType::kIncr, {"counter"}, WriteSource::kClient, true}).ok);
  ASSERT_TRUE(
      dispatcher.Execute({CommandType::kSet, {"counter", "text"}, WriteSource::kClient, true}).ok);
  EXPECT_FALSE(
      dispatcher.Execute({CommandType::kDecr, {"counter"}, WriteSource::kClient, true}).ok);
}

TEST(ProtocolTest, NativeMissingAndEmptyValuesRemainDistinct) {
  Dispatcher dispatcher(std::make_unique<HashEngine>(8));
  ASSERT_TRUE(dispatcher.Execute({CommandType::kSet, {"empty", ""}}).ok);
  const auto empty = dispatcher.Execute({CommandType::kGet, {"empty"}});
  EXPECT_TRUE(empty.ok);
  EXPECT_FALSE(empty.null_value);
  EXPECT_TRUE(empty.value.empty());
  const auto missing = dispatcher.Execute({CommandType::kGet, {"missing"}});
  EXPECT_FALSE(missing.ok);
  EXPECT_NE(EncodeNative(missing).find("NOT_FOUND"), std::string::npos);
  EXPECT_NE(EncodeBatch({missing}).find("NOT_FOUND"), std::string::npos);
}

}  // namespace kvstore
