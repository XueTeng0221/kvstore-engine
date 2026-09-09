#include <gtest/gtest.h>

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

TEST(ProtocolTest, DispatcherRollsBackWhenEventSinkFails) {
  Dispatcher dispatcher(std::make_unique<HashEngine>(16), [](const WriteEvent&) {
    return Status{StatusCode::kIoError, "sink failed"};
  });
  const auto response = dispatcher.Execute({CommandType::kSet, {"key", "value"}});
  EXPECT_FALSE(response.ok);
  EXPECT_TRUE(dispatcher.Execute({CommandType::kGet, {"key"}}).null_value);
  EXPECT_EQ(dispatcher.next_offset(), 1U);
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

}  // namespace kvstore
