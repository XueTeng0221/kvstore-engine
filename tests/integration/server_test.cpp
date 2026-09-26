#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "kvstore/command/dispatcher.hpp"
#include "kvstore/engine/hash_engine.hpp"
#include "kvstore/net/epoll_server.hpp"
#include "kvstore/net/io_uring_server.hpp"
#include "kvstore/net/ntyco_server.hpp"
#include "kvstore/replication/backlog.hpp"
#include "kvstore/replication/executor.hpp"

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

TEST(IoUringServerIntegrationTest, ServesRespAndStopsOrReportsUnsupported) {
  const auto probe = IoUringServer::Probe(32, false);
  if (!probe.ok()) {
    if (probe.code() == StatusCode::kUnsupported)
      GTEST_SKIP() << "io_uring unavailable: " << probe.message();
    FAIL() << "io_uring probe failed unexpectedly: " << probe.message();
  }
  ServerConfig server_config{.listen_address = "127.0.0.1",
                             .port = ReservePort(),
                             .worker_threads = 1,
                             .max_connections = 8,
                             .max_inflight_requests = 8,
                             .graceful_shutdown_ms = 500,
                             .idle_timeout_ms = 1000,
                             .handshake_timeout_ms = 500,
                             .max_input_buffer_bytes = 4096,
                             .max_output_buffer_bytes = 4096,
                             .output_high_watermark_bytes = 2048};
  ProtocolConfig protocol_config{.enabled = {"resp2", "text-kv", "batch-v1"},
                                 .max_key_bytes = 128,
                                 .max_value_bytes = 1024,
                                 .max_frame_bytes = 4096,
                                 .max_batch_records = 16,
                                 .parse_timeout_ms = 500};
  Dispatcher dispatcher(std::make_unique<HashEngine>(64));
  IoUringServer server(server_config, protocol_config, dispatcher, "primary", {}, {}, nullptr, 1000,
                       32, nullptr, false, 32, false, false);
  Status run_status;
  std::jthread thread([&] { run_status = server.Run(); });
  for (int retry = 0; retry < 100 && !server.ready(); ++retry)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_TRUE(server.ready()) << run_status.message();
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(server_config.port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  const std::string ping = "*1\r\n$4\r\nPING\r\n";
  ASSERT_EQ(::send(fd, ping.data(), ping.size(), MSG_NOSIGNAL), static_cast<ssize_t>(ping.size()));
  char response[32]{};
  const auto received = ::recv(fd, response, sizeof(response), 0);
  ASSERT_GT(received, 0);
  EXPECT_NE(std::string_view(response, static_cast<std::size_t>(received)).find("+PONG\r\n"),
            std::string_view::npos);
  ::close(fd);
  server.Stop();
  thread.join();
  EXPECT_TRUE(run_status.ok()) << run_status.message();
}

TEST(NtycoServerIntegrationTest, ServesRespAndStopsOrReportsUnsupported) {
  ServerConfig server_config{.listen_address = "127.0.0.1",
                             .port = ReservePort(),
                             .worker_threads = 1,
                             .max_connections = 8,
                             .max_inflight_requests = 8,
                             .graceful_shutdown_ms = 500,
                             .idle_timeout_ms = 1000,
                             .handshake_timeout_ms = 500,
                             .max_input_buffer_bytes = 4096,
                             .max_output_buffer_bytes = 4096,
                             .output_high_watermark_bytes = 2048};
  ProtocolConfig protocol_config{.enabled = {"resp2", "text-kv", "batch-v1"},
                                 .max_key_bytes = 128,
                                 .max_value_bytes = 1024,
                                 .max_frame_bytes = 4096,
                                 .max_batch_records = 16,
                                 .parse_timeout_ms = 500};
  Dispatcher dispatcher(std::make_unique<HashEngine>(64));
  NtycoServer server(server_config, protocol_config, dispatcher, 32U * 1024U);
  Status run_status;
  std::jthread thread([&] { run_status = server.Run(); });
  for (int retry = 0; retry < 100 && !server.ready() && server.state() != ServerState::kFailed;
       ++retry)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  if (!server.ready()) {
    server.Stop();
    thread.join();
    if (run_status.code() == StatusCode::kUnsupported) GTEST_SKIP() << run_status.message();
    FAIL() << run_status.message();
  }
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(server_config.port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  const std::string ping = "*1\r\n$4\r\nPING\r\n";
  ASSERT_EQ(::send(fd, ping.data(), ping.size(), MSG_NOSIGNAL), static_cast<ssize_t>(ping.size()));
  char response[32]{};
  const auto received = ::recv(fd, response, sizeof(response), 0);
  ASSERT_GT(received, 0);
  EXPECT_NE(std::string_view(response, static_cast<std::size_t>(received)).find("+PONG\r\n"),
            std::string_view::npos);
  ::close(fd);
  const int fragmented_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(fragmented_fd, 0);
  ASSERT_EQ(::connect(fragmented_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  ASSERT_EQ(::send(fragmented_fd, ping.data(), 1, MSG_NOSIGNAL), 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  ASSERT_EQ(::send(fragmented_fd, ping.data() + 1, ping.size() - 1, MSG_NOSIGNAL),
            static_cast<ssize_t>(ping.size() - 1));
  std::memset(response, 0, sizeof(response));
  const auto fragmented_received = ::recv(fragmented_fd, response, sizeof(response), 0);
  ASSERT_GT(fragmented_received, 0);
   EXPECT_NE(std::string_view(response, static_cast<std::size_t>(fragmented_received)).find("+PONG\r\n"),
             std::string_view::npos);
   ::close(fragmented_fd);
  const int hup_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(hup_fd, 0);
  ASSERT_EQ(::connect(hup_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  ::close(hup_fd);
  const int slow_reader_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(slow_reader_fd, 0);
  ASSERT_EQ(::connect(slow_reader_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  std::string pipelined;
  for (int index = 0; index < 64; ++index) pipelined += ping;
  ASSERT_EQ(::send(slow_reader_fd, pipelined.data(), pipelined.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(pipelined.size()));
  const int idle_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(idle_fd, 0);
  ASSERT_EQ(::connect(idle_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  const std::string partial = "*";
  ASSERT_EQ(::send(idle_fd, partial.data(), partial.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(partial.size()));
  const auto stop_started = std::chrono::steady_clock::now();
  server.Stop();
  thread.join();
  const auto stop_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - stop_started);
  EXPECT_LT(stop_elapsed.count(), static_cast<std::int64_t>(server_config.graceful_shutdown_ms));
   EXPECT_TRUE(run_status.ok()) << run_status.message();
   ::close(idle_fd);
  ::close(slow_reader_fd);
}

TEST(IoUringServerIntegrationTest, ServesNativeBatchAndFragmentedResp) {
  const auto probe = IoUringServer::Probe(32, false);
  if (!probe.ok()) {
    if (probe.code() == StatusCode::kUnsupported)
      GTEST_SKIP() << "io_uring unavailable: " << probe.message();
    FAIL() << "io_uring probe failed unexpectedly: " << probe.message();
  }
  ServerConfig server_config{.listen_address = "127.0.0.1",
                             .port = ReservePort(),
                             .worker_threads = 1,
                             .max_connections = 8,
                             .max_inflight_requests = 8,
                             .graceful_shutdown_ms = 500,
                             .idle_timeout_ms = 1000,
                             .handshake_timeout_ms = 500,
                             .max_input_buffer_bytes = 4096,
                             .max_output_buffer_bytes = 4096,
                             .output_high_watermark_bytes = 2048};
  ProtocolConfig protocol_config{.enabled = {"resp2", "text-kv", "batch-v1"},
                                 .max_key_bytes = 128,
                                 .max_value_bytes = 1024,
                                 .max_frame_bytes = 4096,
                                 .max_batch_records = 16,
                                 .parse_timeout_ms = 500};
  Dispatcher dispatcher(std::make_unique<HashEngine>(64));
  IoUringServer server(server_config, protocol_config, dispatcher);
  Status run_status;
  std::jthread thread([&] { run_status = server.Run(); });
  for (int retry = 0; retry < 100 && !server.ready(); ++retry)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_TRUE(server.ready()) << run_status.message();
  const auto connect = [&] {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    EXPECT_GE(fd, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(server_config.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    return fd;
  };
  const auto receive = [](int fd) {
    char response[4096]{};
    const auto count = ::recv(fd, response, sizeof(response), 0);
    return count > 0 ? std::string(response, static_cast<std::size_t>(count)) : std::string{};
  };
  int fd = connect();
  const std::string first = "*1\r\n$4\r\nPING\r\n";
  ASSERT_EQ(::send(fd, first.data(), 5, MSG_NOSIGNAL), 5);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_EQ(::send(fd, first.data() + 5, first.size() - 5, MSG_NOSIGNAL),
            static_cast<ssize_t>(first.size() - 5));
  EXPECT_NE(receive(fd).find("+PONG\r\n"), std::string::npos);
  ::close(fd);

  fd = connect();
  const std::string native = "KV/1 SET 3 3\r\nkeyvalue";
  ASSERT_EQ(::send(fd, native.data(), native.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(native.size()));
  const auto native_response = receive(fd);
  EXPECT_NE(native_response.find("KV/1 OK"), std::string::npos);
  ::close(fd);

  fd = connect();
  const auto batch = BatchFrame(CommandType::kSet, "bkey", "bval");
  ASSERT_EQ(::send(fd, batch.data(), batch.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(batch.size()));
  const auto batch_response = receive(fd);
  ASSERT_GE(batch_response.size(), 20U);
  EXPECT_EQ(batch_response.substr(0, 4), "KVB1");
  EXPECT_EQ(static_cast<unsigned char>(batch_response[4]), 0U);
  EXPECT_EQ(static_cast<unsigned char>(batch_response[5]), 1U);
  EXPECT_EQ(static_cast<unsigned char>(batch_response[8]), 0U);
  EXPECT_EQ(static_cast<unsigned char>(batch_response[11]), 1U);
  ::close(fd);

  server.Stop();
  thread.join();
  EXPECT_TRUE(run_status.ok()) << run_status.message();
}

TEST(IoUringServerIntegrationTest, StopsWithPendingPartialRequest) {
  const auto probe = IoUringServer::Probe(32, false);
  if (!probe.ok()) {
    if (probe.code() == StatusCode::kUnsupported)
      GTEST_SKIP() << "io_uring unavailable: " << probe.message();
    FAIL() << "io_uring probe failed unexpectedly: " << probe.message();
  }
  ServerConfig server_config{.listen_address = "127.0.0.1",
                             .port = ReservePort(),
                             .worker_threads = 1,
                             .max_connections = 2,
                             .max_inflight_requests = 2,
                             .graceful_shutdown_ms = 500,
                             .idle_timeout_ms = 1000,
                             .handshake_timeout_ms = 500,
                             .max_input_buffer_bytes = 256,
                             .max_output_buffer_bytes = 512,
                             .output_high_watermark_bytes = 256};
  ProtocolConfig protocol_config{.enabled = {"resp2"},
                                 .max_key_bytes = 64,
                                 .max_value_bytes = 128,
                                 .max_frame_bytes = 256,
                                 .max_batch_records = 4,
                                 .parse_timeout_ms = 1000};
  Dispatcher dispatcher(std::make_unique<HashEngine>(16));
  IoUringServer server(server_config, protocol_config, dispatcher);
  Status run_status;
  std::jthread thread([&] { run_status = server.Run(); });
  for (int retry = 0; retry < 100 && !server.ready(); ++retry)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_TRUE(server.ready()) << run_status.message();
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(server_config.port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  const std::string partial = "*1\r\n$4\r\nPING";
  ASSERT_EQ(::send(fd, partial.data(), partial.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(partial.size()));
  server.Stop();
  thread.join();
  ::close(fd);
  EXPECT_TRUE(run_status.ok()) << run_status.message();
}

TEST(ReplicationSocketIntegrationTest, PrimaryReplicaConvergeAndReconnect) {
  const auto primary_port = ReservePort();
  const auto replica_port = ReservePort();
  ServerConfig primary_config{"127.0.0.1", primary_port, 1,         16,        8,        1000,
                              3000,        500,          1U << 20U, 1U << 20U, 1U << 19U};
  ServerConfig replica_config{"127.0.0.1", replica_port, 1,         16,        8,        1000,
                              3000,        500,          1U << 20U, 1U << 20U, 1U << 19U};
  ProtocolConfig protocol{{"resp2"}, 128, 1024, 1U << 20U, 16, 500};
  ReplicationBacklog backlog;
  Dispatcher primary_dispatcher(
      std::make_unique<HashEngine>(64),
      [&backlog](const WriteEvent& event) { return backlog.Append({event}); });
  Dispatcher replica_dispatcher(std::make_unique<HashEngine>(64));
  replica_dispatcher.SetReadOnly(true);
  EpollServer primary(primary_config, protocol, primary_dispatcher, "primary", "p1", {}, &backlog,
                      100, 128);
  auto replica =
      std::make_unique<EpollServer>(replica_config, protocol, replica_dispatcher, "replica", "r1",
                                    "127.0.0.1:" + std::to_string(primary_port), nullptr, 100, 128);
  std::jthread primary_thread([&] { EXPECT_TRUE(primary.Run().ok()); });
  for (int retry = 0; retry < 100 && !primary.ready(); ++retry)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_TRUE(primary.ready());
  std::jthread replica_thread([&] { EXPECT_TRUE(replica->Run().ok()); });
  for (int retry = 0; retry < 100 && !replica->ready(); ++retry)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_TRUE(replica->ready());

  const auto execute_set = [](Dispatcher& dispatcher, std::string key, std::string value) {
    return dispatcher
        .Execute(
            {CommandType::kSet, {std::move(key), std::move(value)}, WriteSource::kClient, true})
        .ok;
  };
  const auto wait_for_value = [](Dispatcher& dispatcher, std::string_view key,
                                 std::string_view expected, int attempts) {
    for (int retry = 0; retry < attempts; ++retry) {
      const auto value = dispatcher.engine().Get(key, {});
      if (value.ok() && value.value() == expected) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  };
  EXPECT_TRUE(execute_set(primary_dispatcher, "before", "one"));
  EXPECT_TRUE(execute_set(primary_dispatcher, "after", "two"));
  EXPECT_TRUE(wait_for_value(replica_dispatcher, "before", "one", 100));
  EXPECT_TRUE(wait_for_value(replica_dispatcher, "after", "two", 100));

  primary.Stop();
  primary_thread.join();
  EXPECT_TRUE(execute_set(primary_dispatcher, "during", "three"));
  EpollServer restarted_primary(primary_config, protocol, primary_dispatcher, "primary", "p1", {},
                                &backlog, 100, 128);
  std::jthread restarted_primary_thread([&] { EXPECT_TRUE(restarted_primary.Run().ok()); });
  for (int retry = 0; retry < 100 && !restarted_primary.ready(); ++retry)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_TRUE(restarted_primary.ready());
  const bool recovered = wait_for_value(replica_dispatcher, "during", "three", 300);
  EXPECT_TRUE(recovered) << "same replica instance did not resume upstream replication";
  replica->Stop();
  replica_thread.join();
  restarted_primary.Stop();
  restarted_primary_thread.join();
}

TEST(ReplicationSocketIntegrationTest, ThreeNodeRelayPreservesOriginWithoutDuplicateForwarding) {
  const auto primary_port = ReservePort();
  const auto relay_port = ReservePort();
  const auto leaf_port = ReservePort();
  const auto server_config = [](std::uint16_t port) {
    return ServerConfig{"127.0.0.1", port, 1,         16,        8,        1000,
                        3000,        500,  1U << 20U, 1U << 20U, 1U << 19U};
  };
  const ProtocolConfig protocol{{"resp2"}, 128, 1024, 1U << 20U, 16, 500};
  ReplicationBacklog primary_backlog;
  ReplicationBacklog relay_backlog;
  Dispatcher primary_dispatcher(
      std::make_unique<HashEngine>(64),
      [&primary_backlog](const WriteEvent& event) { return primary_backlog.Append({event}); }, {},
      {}, 1, 1, "node-a");
  Dispatcher relay_dispatcher(std::make_unique<HashEngine>(64));
  Dispatcher leaf_dispatcher(std::make_unique<HashEngine>(64));
  relay_dispatcher.SetReadOnly(true);
  leaf_dispatcher.SetReadOnly(true);
  EXPECT_TRUE(
      primary_dispatcher.Execute({CommandType::kSet, {"seed", "ready"}, WriteSource::kClient, true})
          .ok);

  auto primary_config = server_config(primary_port);
  auto relay_config = server_config(relay_port);
  auto leaf_config = server_config(leaf_port);
  EpollServer primary(primary_config, protocol, primary_dispatcher, "primary", "node-a", {},
                      &primary_backlog, 100, 128);
  EpollServer relay(relay_config, protocol, relay_dispatcher, "replica", "node-b",
                    "127.0.0.1:" + std::to_string(primary_port), &relay_backlog, 100, 128);
  EpollServer leaf(leaf_config, protocol, leaf_dispatcher, "replica", "node-c",
                   "127.0.0.1:" + std::to_string(relay_port), nullptr, 100, 128);
  std::jthread primary_thread([&] { EXPECT_TRUE(primary.Run().ok()); });
  std::jthread relay_thread([&] { EXPECT_TRUE(relay.Run().ok()); });
  std::jthread leaf_thread([&] { EXPECT_TRUE(leaf.Run().ok()); });
  for (int retry = 0; retry < 100 && (!primary.ready() || !relay.ready() || !leaf.ready()); ++retry)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  bool topology_ready = false;
  for (int retry = 0; retry < 300; ++retry) {
    const auto relay_seed = relay_dispatcher.engine().Get("seed", {});
    const auto leaf_seed = leaf_dispatcher.engine().Get("seed", {});
    if (relay_seed.ok() && leaf_seed.ok()) {
      topology_ready = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(topology_ready);

  const auto response = primary_dispatcher.Execute(
      {CommandType::kSet, {"relay-key", "relay-value"}, WriteSource::kClient, true});
  EXPECT_TRUE(response.ok);
  bool converged = false;
  for (int retry = 0; retry < 300; ++retry) {
    const auto value = leaf_dispatcher.engine().Get("relay-key", {});
    if (value.ok() && value.value() == "relay-value") {
      converged = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(converged);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(primary_backlog.size(), 2U);
  EXPECT_LE(relay_backlog.size(), 1U);
  if (relay_backlog.size() == 1U) {
    const auto forwarded = relay_backlog.ReadOne(2);
    EXPECT_TRUE(forwarded.ok());
    if (forwarded.ok()) {
      EXPECT_EQ(forwarded.value().origin_node, "node-a");
      EXPECT_EQ(forwarded.value().event_id, 2U);
    }
  }

  leaf.Stop();
  relay.Stop();
  primary.Stop();
  leaf_thread.join();
  relay_thread.join();
  primary_thread.join();
}

TEST(ReplicationSocketIntegrationTest, BidirectionalCycleDoesNotAmplifyOriginEvent) {
  const auto a_port = ReservePort();
  const auto b_port = ReservePort();
  const auto make_config = [](std::uint16_t port) {
    return ServerConfig{"127.0.0.1", port, 1,         16,        8,        1000,
                        3000,        500,  1U << 20U, 1U << 20U, 1U << 19U};
  };
  const ProtocolConfig protocol{{"resp2"}, 128, 1024, 1U << 20U, 16, 500};
  ReplicationBacklog a_backlog;
  ReplicationBacklog b_backlog;
  Dispatcher a_dispatcher(
      std::make_unique<HashEngine>(64),
      [&a_backlog](const WriteEvent& event) { return a_backlog.Append({event}); }, {}, {}, 1, 1,
      "node-a");
  Dispatcher b_dispatcher(
      std::make_unique<HashEngine>(64),
      [&b_backlog](const WriteEvent& event) { return b_backlog.Append({event}); }, {}, {}, 1, 1,
      "node-b");
  b_dispatcher.SetReadOnly(true);
  ASSERT_TRUE(
      a_dispatcher.Execute({CommandType::kSet, {"seed", "ready"}, WriteSource::kClient, true}).ok);
  auto a_config = make_config(a_port);
  auto b_config = make_config(b_port);
  EpollServer a(a_config, protocol, a_dispatcher, "primary", "node-a",
                "127.0.0.1:" + std::to_string(b_port), &a_backlog, 100, 128, nullptr, true);
  EpollServer b(b_config, protocol, b_dispatcher, "replica", "node-b",
                "127.0.0.1:" + std::to_string(a_port), &b_backlog, 100, 128, nullptr, true);
  std::jthread a_thread([&] { EXPECT_TRUE(a.Run().ok()); });
  std::jthread b_thread([&] { EXPECT_TRUE(b.Run().ok()); });
  for (int retry = 0; retry < 100 && (!a.ready() || !b.ready()); ++retry)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  if (!a.ready() || !b.ready()) {
    a.Stop();
    b.Stop();
    b_thread.join();
    a_thread.join();
    ADD_FAILURE() << "bidirectional cycle servers did not become ready";
    return;
  }
  bool seed_converged = false;
  for (int retry = 0; retry < 300; ++retry) {
    const auto value = b_dispatcher.engine().Get("seed", {});
    if (value.ok() && value.value() == "ready") {
      seed_converged = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!seed_converged) {
    a.Stop();
    b.Stop();
    b_thread.join();
    a_thread.join();
    ADD_FAILURE() << "initial snapshot did not converge";
    return;
  }
  if (!a_dispatcher.Execute({CommandType::kSet, {"cycle", "value"}, WriteSource::kClient, true})
           .ok) {
    a.Stop();
    b.Stop();
    b_thread.join();
    a_thread.join();
    ADD_FAILURE() << "primary write failed";
    return;
  }
  bool converged = false;
  for (int retry = 0; retry < 300; ++retry) {
    const auto value = b_dispatcher.engine().Get("cycle", {});
    if (value.ok() && value.value() == "value") {
      converged = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(converged);
  EXPECT_EQ(a_backlog.size(), 2U);
  EXPECT_LE(b_backlog.size(), 1U);
  EXPECT_GE(a_dispatcher.next_offset(), 3U);
  EXPECT_GE(b_dispatcher.next_offset(), 3U);
  const auto a_event = a_backlog.ReadOne(2);
  if (!a_event.ok()) {
    b.Stop();
    a.Stop();
    b_thread.join();
    a_thread.join();
    ADD_FAILURE() << "cycle event was not present in primary backlog";
    return;
  }
  EXPECT_EQ(a_event.value().origin_node, "node-a");
  EXPECT_EQ(a_event.value().key, "cycle");
  EXPECT_EQ(a_event.value().value, "value");
  b.Stop();
  a.Stop();
  b_thread.join();
  a_thread.join();
}

TEST(ReplicationSocketIntegrationTest, AllExecutorsDriveSocketReplicaApply) {
  for (const std::string backend : {"pthread", "reactor", "proactor", "ntyco"}) {
    ReplicationExecutorOptions options;
    options.queue_capacity = 8;
    options.reactor_post = [](std::function<void()> task) {
      task();
      return Status::Ok();
    };
    options.proactor_submit = [](std::function<void()> task,
                                 ProactorReplicationExecutor::Completion completion) {
      task();
      completion();
      return Status::Ok();
    };
    auto executor = CreateReplicationExecutor(backend, std::move(options));
    ASSERT_TRUE(executor.ok()) << backend;
    const auto primary_port = ReservePort();
    const auto replica_port = ReservePort();
    ServerConfig primary_config{"127.0.0.1", primary_port, 1,         16,        8,        1000,
                                3000,        500,          1U << 20U, 1U << 20U, 1U << 19U};
    ServerConfig replica_config{"127.0.0.1", replica_port, 1,         16,        8,        1000,
                                3000,        500,          1U << 20U, 1U << 20U, 1U << 19U};
    ProtocolConfig protocol{{"resp2"}, 128, 1024, 1U << 20U, 16, 500};
    ReplicationBacklog backlog;
    Dispatcher primary_dispatcher(
        std::make_unique<HashEngine>(64),
        [&backlog](const WriteEvent& event) { return backlog.Append({event}); }, {}, {}, 1, 1,
        "primary");
    Dispatcher replica_dispatcher(std::make_unique<HashEngine>(64));
    replica_dispatcher.SetReadOnly(true);
    EpollServer primary(primary_config, protocol, primary_dispatcher, "primary", "primary", {},
                        &backlog, 100, 128);
    EpollServer replica(replica_config, protocol, replica_dispatcher, "replica", "replica",
                        "127.0.0.1:" + std::to_string(primary_port), nullptr, 100, 128,
                        executor.value().get());
    std::jthread primary_thread([&] { EXPECT_TRUE(primary.Run().ok()) << backend; });
    std::jthread replica_thread([&] { EXPECT_TRUE(replica.Run().ok()) << backend; });
    for (int retry = 0; retry < 100 && (!primary.ready() || !replica.ready()); ++retry)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(primary_dispatcher
                    .Execute({CommandType::kSet, {backend, "value"}, WriteSource::kClient, true})
                    .ok);
    bool converged = false;
    for (int retry = 0; retry < 200; ++retry) {
      const auto value = replica_dispatcher.engine().Get(backend, {});
      if (value.ok() && value.value() == "value") {
        converged = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(converged) << backend;
    primary.Stop();
    primary_thread.join();
    EXPECT_TRUE(
        primary_dispatcher
            .Execute(
                {CommandType::kSet, {backend + "-reconnect", "value"}, WriteSource::kClient, true})
            .ok);
    EpollServer restarted_primary(primary_config, protocol, primary_dispatcher, "primary",
                                  "primary", {}, &backlog, 100, 128);
    std::jthread restarted_thread([&] { EXPECT_TRUE(restarted_primary.Run().ok()) << backend; });
    for (int retry = 0; retry < 100 && !restarted_primary.ready(); ++retry)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    bool reconnected = false;
    for (int retry = 0; retry < 300; ++retry) {
      const auto value = replica_dispatcher.engine().Get(backend + "-reconnect", {});
      if (value.ok() && value.value() == "value") {
        reconnected = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(reconnected) << backend;
    replica.Stop();
    restarted_primary.Stop();
    replica_thread.join();
    restarted_thread.join();
    executor.value()->Shutdown();
  }
}

}  // namespace
}  // namespace kvstore
