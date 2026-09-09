#include "kvstore/net/epoll_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <unordered_map>

#include "kvstore/protocol/batch.hpp"
#include "kvstore/protocol/native.hpp"
#include "kvstore/protocol/resp.hpp"

namespace kvstore {
namespace {

struct Connection {
  enum class Mode { kUnknown, kResp, kNative, kBatch } mode{Mode::kUnknown};
  std::string input;
  std::string output;
  bool peer_closed{false};
  RespParser resp;
  NativeParser native;
  BatchParser batch;
  explicit Connection(const ProtocolConfig& config)
      : resp(config.max_frame_bytes),
        native(config.max_key_bytes, config.max_value_bytes, config.max_frame_bytes),
        batch(config.max_batch_records, config.max_frame_bytes) {}
};

void CloseConnection(int fd, int epoll_fd,
                     std::unordered_map<int, std::unique_ptr<Connection>>& connections) {
  ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
  ::close(fd);
  connections.erase(fd);
}

}  // namespace

EpollServer::EpollServer(const ServerConfig& config, const ProtocolConfig& protocol,
                         Dispatcher& dispatcher)
    : server_config_(config), protocol_config_(protocol), dispatcher_(dispatcher) {}

EpollServer::~EpollServer() {
  if (epoll_fd_ >= 0) ::close(epoll_fd_);
  if (listen_fd_ >= 0) ::close(listen_fd_);
}

void EpollServer::Stop() noexcept { stopping_.store(true, std::memory_order_release); }

Status EpollServer::Run() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) return {StatusCode::kIoError, "socket failed"};
  int reuse = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(server_config_.port);
  if (::inet_pton(AF_INET, server_config_.listen_address.c_str(), &address.sin_addr) != 1 ||
      ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
      ::listen(listen_fd_, 256) < 0) {
    return {StatusCode::kIoError, "bind/listen failed"};
  }
  epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) return {StatusCode::kIoError, "epoll create failed"};
  epoll_event listener_event{.events = EPOLLIN, .data = {.fd = listen_fd_}};
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &listener_event) < 0) {
    return {StatusCode::kIoError, "epoll add failed"};
  }
  std::unordered_map<int, std::unique_ptr<Connection>> connections;
  std::vector<epoll_event> events(protocol_config_.max_batch_records);
  while (!stopping_.load(std::memory_order_acquire)) {
    const int count = ::epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()), 100);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) return {StatusCode::kIoError, "epoll wait failed"};
    for (int index = 0; index < count; ++index) {
      const int fd = events[static_cast<std::size_t>(index)].data.fd;
      if (fd == listen_fd_) {
        while (true) {
          const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
          if (client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return {StatusCode::kIoError, "accept failed"};
          }
          if (connections.size() >= server_config_.max_connections) {
            ::close(client);
            continue;
          }
          connections.emplace(client, std::make_unique<Connection>(protocol_config_));
          epoll_event client_event{.events = EPOLLIN | EPOLLRDHUP | EPOLLET,
                                   .data = {.fd = client}};
          ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client, &client_event);
        }
        continue;
      }
      if ((events[static_cast<std::size_t>(index)].events & (EPOLLERR | EPOLLHUP)) != 0) {
        CloseConnection(fd, epoll_fd_, connections);
        continue;
      }
      auto iterator = connections.find(fd);
      if (iterator == connections.end()) continue;
      auto* connection = iterator->second.get();
      char buffer[8192];
      while (true) {
        const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received == 0) {
          connection->peer_closed = true;
          break;
        }
        if (received < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) break;
          CloseConnection(fd, epoll_fd_, connections);
          break;
        }
        if (connection->mode == Connection::Mode::kUnknown) {
          connection->mode = buffer[0] == '*'
                                 ? Connection::Mode::kResp
                                 : (received >= 4 && std::string_view(buffer, 4) == "KVB1"
                                        ? Connection::Mode::kBatch
                                        : Connection::Mode::kNative);
        }
        const std::string_view received_view(buffer, static_cast<std::size_t>(received));
        Status feed_status;
        if (connection->mode == Connection::Mode::kResp)
          feed_status = connection->resp.Feed(received_view);
        else if (connection->mode == Connection::Mode::kBatch)
          feed_status = connection->batch.Feed(received_view);
        else
          feed_status = connection->native.Feed(received_view);
        if (!feed_status.ok()) {
          connection->output +=
              connection->mode == Connection::Mode::kResp
                  ? EncodeResp({false, {}, std::string(feed_status.message()), false, false, {}})
              : connection->mode == Connection::Mode::kBatch
                  ? EncodeBatch({{false, {}, std::string(feed_status.message()), false, false, {}}})
                  : EncodeNative({false, {}, std::string(feed_status.message()), false, false, {}});
          break;
        }
        Result<std::vector<Command>> parsed =
            connection->mode == Connection::Mode::kResp    ? connection->resp.ParseAvailable()
            : connection->mode == Connection::Mode::kBatch ? connection->batch.ParseAvailable()
                                                           : connection->native.ParseAvailable();
        if (!parsed.ok()) {
          connection->output +=
              connection->mode == Connection::Mode::kResp
                  ? EncodeResp(
                        {false, {}, std::string(parsed.status().message()), false, false, {}})
              : connection->mode == Connection::Mode::kBatch
                  ? EncodeBatch(
                        {{false, {}, std::string(parsed.status().message()), false, false, {}}})
                  : EncodeNative(
                        {false, {}, std::string(parsed.status().message()), false, false, {}});
          break;
        }
        std::vector<CommandResponse> batch_responses;
        for (const auto& command : parsed.value()) {
          const auto response = dispatcher_.Execute(command);
          if (connection->mode == Connection::Mode::kBatch)
            batch_responses.push_back(response);
          else
            connection->output += connection->mode == Connection::Mode::kResp
                                      ? EncodeResp(response)
                                      : EncodeNative(response);
        }
        if (connection->mode == Connection::Mode::kBatch && !batch_responses.empty())
          connection->output += EncodeBatch(batch_responses);
        if (connection->output.size() > protocol_config_.max_frame_bytes * 2U) break;
      }
      if (connections.contains(fd) && !connection->output.empty()) {
        while (!connection->output.empty()) {
          const ssize_t sent =
              ::send(fd, connection->output.data(), connection->output.size(), MSG_NOSIGNAL);
          if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
          if (sent <= 0) {
            CloseConnection(fd, epoll_fd_, connections);
            break;
          }
          connection->output.erase(0, static_cast<std::size_t>(sent));
        }
      }
      if (connections.contains(fd)) {
        if (connection->peer_closed && connection->output.empty()) {
          CloseConnection(fd, epoll_fd_, connections);
          continue;
        }
        epoll_event update{
            .events = static_cast<std::uint32_t>(EPOLLIN | EPOLLRDHUP | EPOLLET) |
                      (!connection->output.empty() ? static_cast<std::uint32_t>(EPOLLOUT) : 0U),
            .data = {.fd = fd}};
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &update);
      }
    }
  }
  for (const auto& [fd, unused] : connections) {
    static_cast<void>(unused);
    ::close(fd);
  }
  connections.clear();
  return Status::Ok();
}

}  // namespace kvstore
