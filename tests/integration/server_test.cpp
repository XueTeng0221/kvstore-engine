#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "kvstore/command/dispatcher.hpp"
#include "kvstore/engine/hash_engine.hpp"
#include "kvstore/net/epoll_server.hpp"

namespace kvstore {
namespace {

template <typename T>
void AppendBigEndian(std::string& output, T value) {
  for (std::size_t index = 0; index < sizeof(T); ++index)
    output.push_back(static_cast<char>(value >> ((sizeof(T) - index - 1U) * 8U)));
}

std::string BatchFrame(CommandType type, std::string_view key, std::string_view value) {
  std::string payload;
  payload.push_back(static_cast<char>(type));
  AppendBigEndian<std::uint32_t>(payload, static_cast<std::uint32_t>(key.size()));
  AppendBigEndian<std::uint64_t>(payload, value.size());
  payload.append(key);
  payload.append(value);
  std::string frame("KVB1", 4);
  AppendBigEndian<std::uint16_t>(frame, 1);
  AppendBigEndian<std::uint16_t>(frame, 0);
  AppendBigEndian<std::uint32_t>(frame, 1);
  AppendBigEndian<std::uint64_t>(frame, payload.size());
  frame += payload;
  return frame;
}

std::uint16_t ReservePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  EXPECT_GE(fd, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  socklen_t length = sizeof(address);
  EXPECT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length), 0);
  ::close(fd);
  return ntohs(address.sin_port);
}

class ServerFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    server_config_ = {.listen_address = "127.0.0.1",
                      .port = ReservePort(),
                      .worker_threads = 4,
                      .max_connections = 32,
                      .max_inflight_requests = 16,
                      .graceful_shutdown_ms = 1000,
                      .idle_timeout_ms = 1000,
                      .handshake_timeout_ms = 200,
                      .max_input_buffer_bytes = 4096,
                      .max_output_buffer_bytes = 4096,
                      .output_high_watermark_bytes = 2048};
    protocol_config_ = {.enabled = {"resp2", "text-kv", "batch-v1"},
                        .max_key_bytes = 128,
                        .max_value_bytes = 1024,
                        .max_frame_bytes = 4096,
                        .max_batch_records = 16,
                        .parse_timeout_ms = 200};
    dispatcher_ = std::make_unique<Dispatcher>(std::make_unique<HashEngine>(64));
    server_ = std::make_unique<EpollServer>(server_config_, protocol_config_, *dispatcher_);
    server_thread_ = std::jthread([this] { run_status_ = server_->Run(); });
    for (int retry = 0; retry < 100 && !server_->ready(); ++retry)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(server_->ready());
  }

  void TearDown() override {
    if (server_) server_->Stop();
    if (server_thread_.joinable()) server_thread_.join();
    EXPECT_TRUE(run_status_.ok()) << run_status_.message();
  }

  int Connect() const {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    EXPECT_GE(fd, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(server_config_.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    return fd;
  }

  static std::string ReadAvailable(int fd) {
    std::string output;
    char buffer[4096];
    pollfd descriptor{.fd = fd, .events = POLLIN, .revents = 0};
    while (::poll(&descriptor, 1, 200) > 0) {
      const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
      if (count <= 0) break;
      output.append(buffer, static_cast<std::size_t>(count));
      descriptor.revents = 0;
    }
    return output;
  }

  ServerConfig server_config_;
  ProtocolConfig protocol_config_;
  std::unique_ptr<Dispatcher> dispatcher_;
  std::unique_ptr<EpollServer> server_;
  std::jthread server_thread_;
  Status run_status_;
};

TEST_F(ServerFixture, RespPipelineUnknownCommandAndClientStateKeepConnectionHealthy) {
  const int fd = Connect();
  const std::string requests =
      "*1\r\n$7\r\nUNKNOWN\r\n"
      "*3\r\n$6\r\nCLIENT\r\n$7\r\nSETNAME\r\n$4\r\ndemo\r\n"
      "*2\r\n$6\r\nCLIENT\r\n$7\r\nGETNAME\r\n"
      "*1\r\n$4\r\nPING\r\n";
  ASSERT_EQ(::send(fd, requests.data(), requests.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(requests.size()));
  const auto response = ReadAvailable(fd);
  EXPECT_NE(response.find("-ERR UNSUPPORTED"), std::string::npos);
  EXPECT_NE(response.find("$4\r\ndemo\r\n"), std::string::npos);
  EXPECT_NE(response.find("+PONG\r\n"), std::string::npos);
  ::close(fd);
}

TEST_F(ServerFixture, HalfCloseDrainsResponseAndMalformedFrameClosesConnection) {
  int fd = Connect();
  const std::string ping = "*1\r\n$4\r\nPING\r\n";
  ASSERT_EQ(::send(fd, ping.data(), ping.size(), MSG_NOSIGNAL), static_cast<ssize_t>(ping.size()));
  ASSERT_EQ(::shutdown(fd, SHUT_WR), 0);
  EXPECT_NE(ReadAvailable(fd).find("+PONG\r\n"), std::string::npos);
  ::close(fd);

  fd = Connect();
  const std::string malformed = "*1\r\n:4\r\nPING\r\n";
  ASSERT_EQ(::send(fd, malformed.data(), malformed.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(malformed.size()));
  EXPECT_NE(ReadAvailable(fd).find("-ERR"), std::string::npos);
  ::close(fd);
}

TEST_F(ServerFixture, StopTransitionsThroughDrainingAndStopped) {
  EXPECT_TRUE(server_->ready());
  server_->Stop();
  EXPECT_EQ(server_->state(), ServerState::kDraining);
  server_thread_.join();
  EXPECT_EQ(server_->state(), ServerState::kStopped);
}

TEST_F(ServerFixture, NativeAndBatchServeBinarySafeCrud) {
  int fd = Connect();
  const std::string native = "KV/1 SET 3 3\r\n" + std::string("a\0b", 3) + std::string("v\0x", 3) +
                             "KV/1 GET 3 0\r\n" + std::string("a\0b", 3);
  ASSERT_EQ(::send(fd, native.data(), native.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(native.size()));
  const auto native_response = ReadAvailable(fd);
  EXPECT_NE(native_response.find("KV/1 OK"), std::string::npos);
  EXPECT_NE(native_response.find(std::string("v\0x", 3)), std::string::npos);
  ::close(fd);

  fd = Connect();
  const auto batch = BatchFrame(CommandType::kSet, "batch-key", "batch-value") +
                     BatchFrame(CommandType::kGet, "batch-key", {});
  ASSERT_EQ(::send(fd, batch.data(), batch.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(batch.size()));
  const auto batch_response = ReadAvailable(fd);
  ASSERT_GE(batch_response.size(), 40U);
  EXPECT_EQ(batch_response.substr(0, 4), "KVB1");
  EXPECT_NE(batch_response.find("batch-value"), std::string::npos);
  ::close(fd);

  fd = Connect();
  const std::string mget = "*3\r\n$4\r\nMGET\r\n$9\r\nbatch-key\r\n$7\r\nmissing\r\n";
  ASSERT_EQ(::send(fd, mget.data(), mget.size(), MSG_NOSIGNAL), static_cast<ssize_t>(mget.size()));
  const auto mget_response = ReadAvailable(fd);
  EXPECT_NE(mget_response.find("*2\r\n$11\r\nbatch-value\r\n$-1\r\n"), std::string::npos);
  ::close(fd);

  fd = Connect();
  const auto batch_reads = BatchFrame(CommandType::kGet, "batch-key", {}) +
                           BatchFrame(CommandType::kGet, "batch-key", {});
  ASSERT_EQ(::send(fd, batch_reads.data(), batch_reads.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(batch_reads.size()));
  const auto batch_reads_response = ReadAvailable(fd);
  const auto first_value = batch_reads_response.find("batch-value");
  ASSERT_NE(first_value, std::string::npos);
  EXPECT_NE(batch_reads_response.find("batch-value", first_value + 1), std::string::npos);
  ::close(fd);
}

TEST_F(ServerFixture, HandshakeAndPartialFrameTimeoutCloseConnections) {
  int fd = Connect();
  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  char byte = 0;
  EXPECT_EQ(::recv(fd, &byte, 1, 0), 0);
  ::close(fd);

  fd = Connect();
  const std::string partial = "*2\r\n$3\r\nGET\r\n$4\r\nke";
  ASSERT_EQ(::send(fd, partial.data(), partial.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(partial.size()));
  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  EXPECT_EQ(::recv(fd, &byte, 1, 0), 0);
  ::close(fd);
}

TEST_F(ServerFixture, SequentialConnectionStormAndBindFailureAreContained) {
  const std::string ping = "*1\r\n$4\r\nPING\r\n";
  for (int connection = 0; connection < 100; ++connection) {
    const int fd = Connect();
    ASSERT_EQ(::send(fd, ping.data(), ping.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(ping.size()));
    char response[16]{};
    const auto count = ::recv(fd, response, sizeof(response), 0);
    ASSERT_GT(count, 0);
    EXPECT_NE(std::string_view(response, static_cast<std::size_t>(count)).find("+PONG\r\n"),
              std::string_view::npos);
    ::close(fd);
  }

  Dispatcher other_dispatcher(std::make_unique<HashEngine>(8));
  EpollServer other(server_config_, protocol_config_, other_dispatcher);
  EXPECT_EQ(other.Run().code(), StatusCode::kIoError);
  EXPECT_EQ(other.state(), ServerState::kFailed);
}

TEST_F(ServerFixture, RespAndBatchRejectConfiguredArgumentLimitsBeforeDispatch) {
  int fd = Connect();
  const std::string key(129, 'k');
  const std::string request = "*3\r\n$3\r\nSET\r\n$129\r\n" + key + "\r\n$1\r\nv\r\n";
  ASSERT_EQ(::send(fd, request.data(), request.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(request.size()));
  EXPECT_NE(ReadAvailable(fd).find("LIMIT_EXCEEDED"), std::string::npos);
  EXPECT_FALSE(dispatcher_->engine().Exists(key, {}).value());
  ::close(fd);

  fd = Connect();
  const auto batch = BatchFrame(CommandType::kSet, key, "v");
  ASSERT_EQ(::send(fd, batch.data(), batch.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(batch.size()));
  EXPECT_NE(ReadAvailable(fd).find("batch record payload"), std::string::npos);
  EXPECT_FALSE(dispatcher_->engine().Exists(key, {}).value());
  ::close(fd);
}

TEST_F(ServerFixture, SlowReaderIsBoundedAndConnectionRecoversAfterDrain) {
  ASSERT_TRUE(
      dispatcher_
          ->Execute(
              {CommandType::kSet, {"large", std::string(1024, 'v')}, WriteSource::kClient, true})
          .ok);
  const int fd = Connect();
  const std::string get = "*2\r\n$3\r\nGET\r\n$5\r\nlarge\r\n";
  std::string pipeline;
  for (int request = 0; request < 32; ++request) pipeline += get;
  ASSERT_EQ(::send(fd, pipeline.data(), pipeline.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(pipeline.size()));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto responses = ReadAvailable(fd);
  std::size_t values = 0;
  for (std::size_t position = 0;
       (position = responses.find("$1024\r\n", position)) != std::string::npos; position += 7)
    ++values;
  EXPECT_EQ(values, 32U);
  const std::string ping = "*1\r\n$4\r\nPING\r\n";
  ASSERT_EQ(::send(fd, ping.data(), ping.size(), MSG_NOSIGNAL), static_cast<ssize_t>(ping.size()));
  EXPECT_NE(ReadAvailable(fd).find("+PONG\r\n"), std::string::npos);
  ::close(fd);
}

TEST_F(ServerFixture, ConcurrentConnectionStormCompletesWithoutFdStateLeak) {
  std::atomic<int> successes{0};
  std::vector<std::jthread> clients;
  for (int connection = 0; connection < 32; ++connection) {
    clients.emplace_back([this, &successes] {
      const int fd = Connect();
      const std::string ping = "*1\r\n$4\r\nPING\r\n";
      if (::send(fd, ping.data(), ping.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(ping.size())) {
        char response[16]{};
        const auto count = ::recv(fd, response, sizeof(response), 0);
        if (count > 0 && std::string_view(response, static_cast<std::size_t>(count)).find("PONG") !=
                             std::string_view::npos)
          ++successes;
      }
      ::close(fd);
    });
  }
  clients.clear();
  EXPECT_EQ(successes.load(), 32);
}

TEST_F(ServerFixture, ShutdownDoesNotDispatchNewWrites) {
  const int fd = Connect();
  const auto before = dispatcher_->next_offset();
  server_->Stop();
  const std::string set = "*3\r\n$3\r\nSET\r\n$4\r\nlate\r\n$5\r\nvalue\r\n";
  static_cast<void>(::send(fd, set.data(), set.size(), MSG_NOSIGNAL));
  server_thread_.join();
  EXPECT_EQ(dispatcher_->next_offset(), before);
  EXPECT_FALSE(dispatcher_->engine().Exists("late", {}).value());
  ::close(fd);
}

}  // namespace
}  // namespace kvstore
