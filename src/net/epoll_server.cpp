#include "kvstore/net/epoll_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <utility>

#include "kvstore/protocol/batch.hpp"
#include "kvstore/protocol/native.hpp"
#include "kvstore/protocol/resp.hpp"

namespace kvstore {
namespace {

struct Connection {
  enum class Mode { kUnknown, kResp, kNative, kBatch } mode{Mode::kUnknown};
  std::string input;
  std::string output;
  std::string probe;
  std::string client_name;
  bool peer_closed{false};
  bool waiting_for_frame{false};
  std::vector<Command> pending_commands;
  std::vector<std::vector<Command>> pending_batches;
  std::size_t pending_bytes{0};
  std::uint64_t connection_id{};
  std::uint64_t next_request_id{1};
  std::chrono::steady_clock::time_point last_activity{std::chrono::steady_clock::now()};
  RespParser resp;
  NativeParser native;
  BatchParser batch;
  explicit Connection(const ProtocolConfig& config, std::size_t max_input_buffer, std::uint64_t id)
      : connection_id(id),
        resp(config.max_frame_bytes),
        native(config.max_key_bytes, config.max_value_bytes, config.max_frame_bytes),
        batch(config.max_batch_records, config.max_frame_bytes, max_input_buffer,
              config.max_key_bytes, config.max_value_bytes) {}
};

void CloseConnection(int fd, int epoll_fd,
                     std::unordered_map<int, std::unique_ptr<Connection>>& connections) {
  ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
  ::close(fd);
  connections.erase(fd);
}

bool Enabled(const ProtocolConfig& config, std::string_view name) {
  return std::find(config.enabled.begin(), config.enabled.end(), name) != config.enabled.end();
}

bool Mutates(CommandType type) {
  return type == CommandType::kSet || type == CommandType::kMod || type == CommandType::kDel ||
         type == CommandType::kIncr || type == CommandType::kDecr || type == CommandType::kIncrBy ||
         type == CommandType::kDecrBy;
}

void DrainPending(Connection& connection, Dispatcher& dispatcher, const ServerConfig& config,
                  const ProtocolConfig& protocol) {
  const auto command_bytes = [](const Command& command) {
    std::size_t bytes = 1;
    for (const auto& argument : command.args) bytes += argument.size();
    return bytes;
  };
  const auto process = [&dispatcher, &connection, &protocol](Command command,
                                                             std::size_t response_budget) {
    if (connection.mode == Connection::Mode::kResp && command.type == CommandType::kClient &&
        !command.args.empty()) {
      std::string subcommand = command.args[0];
      for (char& character : subcommand)
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
      if (subcommand == "GETNAME" && command.args.size() == 1)
        command.args.push_back(connection.client_name);
      else if (subcommand == "SETNAME" && command.args.size() == 2)
        connection.client_name = command.args[1];
    }
    RequestContext context;
    context.request_id = connection.next_request_id++;
    context.connection_id = connection.connection_id;
    context.deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(protocol.parse_timeout_ms);
    context.response_budget = response_budget;
    const bool takes_value = command.type == CommandType::kSet || command.type == CommandType::kMod;
    for (std::size_t index = 0; index < command.args.size(); ++index) {
      const auto limit =
          takes_value && index == 1 ? protocol.max_value_bytes : protocol.max_key_bytes;
      if (command.args[index].size() > limit)
        return CommandResponse{false, {}, "LIMIT_EXCEEDED command argument", false, false, {}};
    }
    return dispatcher.Execute(command, context);
  };
  if (connection.mode == Connection::Mode::kBatch) {
    while (!connection.pending_batches.empty()) {
      const auto available = config.max_output_buffer_bytes -
                             std::min(config.max_output_buffer_bytes, connection.output.size());
      auto frame = std::move(connection.pending_batches.front());
      connection.pending_batches.erase(connection.pending_batches.begin());
      for (const auto& command : frame) connection.pending_bytes -= command_bytes(command);
      std::vector<CommandResponse> responses;
      std::size_t used = 20;
      for (auto& command : frame) {
        const auto remaining = available - std::min(available, used);
        CommandResponse response;
        if (Mutates(command.type) && remaining < 128U)
          response = {false, {}, "BUSY response exceeds connection limit", false, false, {}};
        else
          response = process(std::move(command), remaining);
        const auto encoded_size = EncodeBatch({response}).size() - 20U;
        if (encoded_size > available - std::min(available, used))
          response = {false, {}, "BUSY response exceeds connection limit", false, false, {}};
        used += EncodeBatch({response}).size() - 20U;
        responses.push_back(std::move(response));
      }
      connection.output += EncodeBatch(responses);
      if (connection.output.size() >= config.output_high_watermark_bytes) break;
    }
  } else {
    std::size_t processed = 0;
    while (!connection.pending_commands.empty() && processed < config.max_inflight_requests) {
      const auto available = config.max_output_buffer_bytes -
                             std::min(config.max_output_buffer_bytes, connection.output.size());
      if (Mutates(connection.pending_commands.front().type) && available < 128U) break;
      auto command = std::move(connection.pending_commands.front());
      connection.pending_commands.erase(connection.pending_commands.begin());
      connection.pending_bytes -= command_bytes(command);
      connection.output += connection.mode == Connection::Mode::kResp
                               ? EncodeResp(process(std::move(command), available))
                               : EncodeNative(process(std::move(command), available));
      ++processed;
      if (connection.output.size() >= config.output_high_watermark_bytes) break;
    }
  }
}

}  // namespace

EpollServer::EpollServer(const ServerConfig& config, const ProtocolConfig& protocol,
                         Dispatcher& dispatcher, std::size_t max_events)
    : server_config_(config),
      protocol_config_(protocol),
      dispatcher_(dispatcher),
      max_events_(max_events) {}

EpollServer::~EpollServer() {
  if (epoll_fd_ >= 0) ::close(epoll_fd_);
  if (listen_fd_ >= 0) ::close(listen_fd_);
}

void EpollServer::Stop() noexcept {
  state_.store(ServerState::kDraining, std::memory_order_release);
  stopping_.store(true, std::memory_order_release);
}

Status EpollServer::Run() {
  state_.store(ServerState::kStarting, std::memory_order_release);
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) {
    state_.store(ServerState::kFailed, std::memory_order_release);
    return {StatusCode::kIoError, "socket failed"};
  }
  int reuse = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(server_config_.port);
  if (::inet_pton(AF_INET, server_config_.listen_address.c_str(), &address.sin_addr) != 1 ||
      ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
      ::listen(listen_fd_, 256) < 0) {
    state_.store(ServerState::kFailed, std::memory_order_release);
    return {StatusCode::kIoError, "bind/listen failed"};
  }
  epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) {
    state_.store(ServerState::kFailed, std::memory_order_release);
    return {StatusCode::kIoError, "epoll create failed"};
  }
  epoll_event listener_event{.events = EPOLLIN, .data = {.fd = listen_fd_}};
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &listener_event) < 0) {
    state_.store(ServerState::kFailed, std::memory_order_release);
    return {StatusCode::kIoError, "epoll add failed"};
  }
  state_.store(ServerState::kReady, std::memory_order_release);
  std::unordered_map<int, std::unique_ptr<Connection>> connections;
  std::uint64_t next_connection_id = 1;
  std::vector<epoll_event> events(max_events_);
  if (events.empty()) return {StatusCode::kInvalidArgument, "epoll event limit"};
  const auto shutdown_started = std::chrono::steady_clock::time_point{};
  auto shutdown_deadline = shutdown_started;
  while (true) {
    if (stopping_.load(std::memory_order_acquire)) {
      if (shutdown_deadline == shutdown_started)
        shutdown_deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(server_config_.graceful_shutdown_ms);
      if (listen_fd_ >= 0) {
        static_cast<void>(::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, listen_fd_, nullptr));
        ::close(listen_fd_);
        listen_fd_ = -1;
      }
      for (const auto& [fd, connection] : connections) {
        if (connection->output.size() < server_config_.output_high_watermark_bytes)
          DrainPending(*connection, dispatcher_, server_config_, protocol_config_);
        epoll_event drain{
            .events = static_cast<std::uint32_t>(EPOLLRDHUP | EPOLLET) |
                      (!connection->output.empty() ? static_cast<std::uint32_t>(EPOLLOUT) : 0U),
            .data = {.fd = fd}};
        static_cast<void>(::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &drain));
      }
      if (connections.empty() || std::chrono::steady_clock::now() >= shutdown_deadline) break;
    }
    const int count = ::epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()), 100);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) {
      state_.store(ServerState::kFailed, std::memory_order_release);
      return {StatusCode::kIoError, "epoll wait failed"};
    }
    std::vector<int> expired;
    const auto scan_time = std::chrono::steady_clock::now();
    for (const auto& [fd, connection] : connections) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          scan_time - connection->last_activity);
      const auto limit = connection->mode == Connection::Mode::kUnknown
                             ? server_config_.handshake_timeout_ms
                             : (connection->waiting_for_frame ? protocol_config_.parse_timeout_ms
                                                              : server_config_.idle_timeout_ms);
      if (elapsed.count() > static_cast<std::int64_t>(limit)) expired.push_back(fd);
    }
    for (const int fd : expired) CloseConnection(fd, epoll_fd_, connections);
    for (int index = 0; index < count; ++index) {
      const int fd = events[static_cast<std::size_t>(index)].data.fd;
      if (fd == listen_fd_) {
        while (true) {
          const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
          if (client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            state_.store(ServerState::kFailed, std::memory_order_release);
            return {StatusCode::kIoError, "accept failed"};
          }
          if (connections.size() >= server_config_.max_connections) {
            ::close(client);
            continue;
          }
          connections.emplace(client, std::make_unique<Connection>(
                                          protocol_config_, server_config_.max_input_buffer_bytes,
                                          next_connection_id++));
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
      if ((events[static_cast<std::size_t>(index)].events & EPOLLRDHUP) != 0)
        connection->peer_closed = true;
      const auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::milliseconds>(now - connection->last_activity)
              .count() > static_cast<std::int64_t>(server_config_.idle_timeout_ms)) {
        CloseConnection(fd, epoll_fd_, connections);
        continue;
      }
      char buffer[8192];
      const bool readable = !stopping_.load(std::memory_order_acquire) &&
                            (events[static_cast<std::size_t>(index)].events & EPOLLIN) != 0;
      if (readable)
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
          connection->last_activity = std::chrono::steady_clock::now();
          const auto received_size = static_cast<std::size_t>(received);
          if (received_size > server_config_.max_input_buffer_bytes ||
              connection->probe.size() > server_config_.max_input_buffer_bytes - received_size) {
            CloseConnection(fd, epoll_fd_, connections);
            break;
          }
          connection->probe.append(buffer, static_cast<std::size_t>(received));
          if (connection->mode == Connection::Mode::kUnknown) {
            if (connection->probe.front() == '*') {
              if (!Enabled(protocol_config_, "resp2")) {
                CloseConnection(fd, epoll_fd_, connections);
                break;
              }
              connection->mode = Connection::Mode::kResp;
            } else if (connection->probe.size() < 4 && connection->probe.front() == 'K') {
              continue;
            } else if (connection->probe.starts_with("KVB1")) {
              if (!Enabled(protocol_config_, "batch-v1")) {
                CloseConnection(fd, epoll_fd_, connections);
                break;
              }
              connection->mode = Connection::Mode::kBatch;
            } else {
              if (!Enabled(protocol_config_, "text-kv")) {
                CloseConnection(fd, epoll_fd_, connections);
                break;
              }
              connection->mode = Connection::Mode::kNative;
            }
          }
          std::string received_bytes = std::exchange(connection->probe, {});
          const std::string_view received_view = received_bytes;
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
                    ? EncodeBatch(
                          {{false, {}, std::string(feed_status.message()), false, false, {}}})
                    : EncodeNative(
                          {false, {}, std::string(feed_status.message()), false, false, {}});
            connection->peer_closed = true;
            break;
          }
          Result<std::vector<Command>> parsed = connection->mode == Connection::Mode::kResp
                                                    ? connection->resp.ParseAvailable()
                                                    : connection->native.ParseAvailable();
          Result<std::vector<std::vector<Command>>> parsed_frames(
              std::vector<std::vector<Command>>{});
          if (connection->mode == Connection::Mode::kBatch) {
            parsed_frames = connection->batch.ParseAvailableFrames();
            if (!parsed_frames.ok()) {
              connection->output += EncodeBatch(
                  {{false, {}, std::string(parsed_frames.status().message()), false, false, {}}});
              connection->peer_closed = true;
              break;
            }
          }
          if (connection->mode != Connection::Mode::kBatch && !parsed.ok()) {
            connection->output +=
                connection->mode == Connection::Mode::kResp
                    ? EncodeResp(
                          {false, {}, std::string(parsed.status().message()), false, false, {}})
                : connection->mode == Connection::Mode::kBatch
                    ? EncodeBatch(
                          {{false, {}, std::string(parsed.status().message()), false, false, {}}})
                    : EncodeNative(
                          {false, {}, std::string(parsed.status().message()), false, false, {}});
            connection->peer_closed = true;
            break;
          }
          if (connection->mode == Connection::Mode::kBatch) {
            std::size_t incoming_bytes = 0;
            bool exceeds_limit = false;
            for (const auto& frame : parsed_frames.value()) {
              for (const auto& command : frame) {
                std::size_t command_size = 1;
                for (const auto& argument : command.args) {
                  if (argument.size() >
                      server_config_.max_input_buffer_bytes -
                          std::min(server_config_.max_input_buffer_bytes, command_size)) {
                    exceeds_limit = true;
                    break;
                  }
                  command_size += argument.size();
                }
                if (exceeds_limit ||
                    incoming_bytes >
                        server_config_.max_input_buffer_bytes - connection->pending_bytes ||
                    command_size > server_config_.max_input_buffer_bytes -
                                       connection->pending_bytes - incoming_bytes) {
                  exceeds_limit = true;
                  break;
                }
                incoming_bytes += command_size;
              }
              if (exceeds_limit) break;
            }
            if (exceeds_limit) {
              connection->output +=
                  EncodeBatch({{false, {}, "BUSY request queue limit", false, false, {}}});
              connection->peer_closed = true;
              break;
            }
            for (auto& frame : parsed_frames.value()) {
              for (const auto& command : frame) {
                connection->pending_bytes += 1;
                for (const auto& argument : command.args)
                  connection->pending_bytes += argument.size();
              }
              connection->pending_batches.push_back(std::move(frame));
            }
            connection->waiting_for_frame = connection->batch.has_buffered_data();
          } else {
            std::size_t incoming_bytes = 0;
            bool exceeds_limit = false;
            for (const auto& command : parsed.value()) {
              std::size_t command_size = 1;
              for (const auto& argument : command.args) {
                if (argument.size() >
                    server_config_.max_input_buffer_bytes -
                        std::min(server_config_.max_input_buffer_bytes, command_size)) {
                  exceeds_limit = true;
                  break;
                }
                command_size += argument.size();
              }
              if (exceeds_limit ||
                  incoming_bytes >
                      server_config_.max_input_buffer_bytes - connection->pending_bytes ||
                  command_size > server_config_.max_input_buffer_bytes - connection->pending_bytes -
                                     incoming_bytes) {
                exceeds_limit = true;
                break;
              }
              incoming_bytes += command_size;
            }
            if (exceeds_limit) {
              connection->output +=
                  connection->mode == Connection::Mode::kResp
                      ? EncodeResp({false, {}, "BUSY request queue limit", false, false, {}})
                      : EncodeNative({false, {}, "BUSY request queue limit", false, false, {}});
              connection->peer_closed = true;
              break;
            }
            for (auto& command : parsed.value()) {
              connection->pending_bytes += 1;
              for (const auto& argument : command.args)
                connection->pending_bytes += argument.size();
              connection->pending_commands.push_back(std::move(command));
            }
            connection->waiting_for_frame =
                connection->resp.has_buffered_data() || connection->native.has_buffered_data();
          }
          DrainPending(*connection, dispatcher_, server_config_, protocol_config_);
          if (connection->output.size() > server_config_.max_output_buffer_bytes) {
            CloseConnection(fd, epoll_fd_, connections);
            break;
          }
          if (connection->output.size() >= server_config_.output_high_watermark_bytes) break;
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
          connection->last_activity = std::chrono::steady_clock::now();
        }
      }
      if (connections.contains(fd)) {
        if (connection->output.size() < server_config_.output_high_watermark_bytes &&
            (!connection->pending_commands.empty() || !connection->pending_batches.empty()))
          DrainPending(*connection, dispatcher_, server_config_, protocol_config_);
        const bool has_pending =
            !connection->pending_commands.empty() || !connection->pending_batches.empty();
        if ((connection->peer_closed || stopping_.load(std::memory_order_acquire)) &&
            connection->output.empty() && !has_pending) {
          CloseConnection(fd, epoll_fd_, connections);
          continue;
        }
        const bool read_paused =
            connection->output.size() >= server_config_.output_high_watermark_bytes || has_pending;
        epoll_event update{
            .events =
                static_cast<std::uint32_t>(EPOLLRDHUP | EPOLLET) |
                (!read_paused && !connection->peer_closed ? static_cast<std::uint32_t>(EPOLLIN)
                                                          : 0U) |
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
  state_.store(ServerState::kStopped, std::memory_order_release);
  return Status::Ok();
}

}  // namespace kvstore
