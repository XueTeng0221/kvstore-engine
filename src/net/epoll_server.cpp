#include "kvstore/net/epoll_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <utility>

#include "kvstore/protocol/batch.hpp"
#include "kvstore/protocol/native.hpp"
#include "kvstore/protocol/resp.hpp"
#include "kvstore/replication/frame.hpp"
#include "kvstore/replication/handshake.hpp"
#include "kvstore/replication/sync.hpp"

namespace kvstore {
namespace {

constexpr std::size_t kReplicationControlReserve = 1024U;

struct Connection {
  enum class Mode { kUnknown, kResp, kNative, kBatch, kReplication } mode{Mode::kUnknown};
  std::string input;
  std::string output;
  std::deque<std::string> replication_output_queue;
  std::deque<std::string> replication_stream_queue;
  std::unique_ptr<ReplicationChunkGenerator> replication_stream_generator;
  std::size_t replication_queued_bytes{0};
  std::size_t replication_stream_budget{0};
  std::uint64_t replication_stream_final_offset{0};
  std::uint64_t replication_sent_offset{0};
  bool replication_stream_failed{false};
  std::string probe;
  std::string client_name;
  bool peer_closed{false};
  bool upstream{false};
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
  std::unique_ptr<ReplicationConnection> replication;
  std::string replication_input;
  std::vector<ReplicationFrame> replication_chunks;
  std::size_t replication_chunk_bytes{0};
  std::unique_ptr<ReplicaSyncApplier> replica_applier;
  std::uint64_t replication_next_offset{1};
  std::uint64_t replication_ack_offset{0};
  std::uint64_t replication_snapshot_offset{0};
  std::uint64_t replication_snapshot_event_id{0};
  std::uint64_t replication_transfer_id{1};
  std::chrono::steady_clock::time_point last_replication_heartbeat{
      std::chrono::steady_clock::now()};
  std::chrono::steady_clock::time_point last_peer_heartbeat{std::chrono::steady_clock::now()};
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

void AbortReplicationTransfer(Connection& connection) {
  connection.output.clear();
  connection.replication_output_queue.clear();
  connection.replication_stream_queue.clear();
  connection.replication_stream_generator.reset();
  connection.replication_queued_bytes = 0;
  connection.replication_stream_final_offset = 0;
  connection.peer_closed = true;
}

bool Enabled(const ProtocolConfig& config, std::string_view name) {
  return std::find(config.enabled.begin(), config.enabled.end(), name) != config.enabled.end();
}

bool Mutates(CommandType type) {
  return type == CommandType::kSet || type == CommandType::kMod || type == CommandType::kDel ||
         type == CommandType::kIncr || type == CommandType::kDecr || type == CommandType::kIncrBy ||
         type == CommandType::kDecrBy;
}

std::vector<WriteEvent> IncrementalEvents(std::vector<WriteEvent> events) {
  for (auto& event : events) {
    event.source = WriteSource::kIncrementalSync;
    event.checksum = event.ComputeChecksum();
  }
  return events;
}

Status ExecuteReplicationTask(IReplicationExecutor* executor, std::function<Status()> task) {
  if (executor == nullptr) return task();
  return executor->Execute(std::move(task));
}

void PumpReplicationOutput(Connection& connection, std::size_t max_output_bytes) {
  while (!connection.replication_output_queue.empty()) {
    const auto& frame = connection.replication_output_queue.front();
    if (frame.size() > max_output_bytes - std::min(max_output_bytes, connection.output.size()))
      break;
    connection.output.append(frame);
    connection.replication_queued_bytes -= frame.size();
    connection.replication_output_queue.pop_front();
  }
  while (true) {
    const auto budget = connection.replication_stream_budget == 0
                            ? max_output_bytes
                            : connection.replication_stream_budget;
    if (!connection.replication_stream_queue.empty()) {
      const auto& frame = connection.replication_stream_queue.front();
      if (frame.size() > budget - std::min(budget, connection.output.size())) break;
      connection.output.append(frame);
      connection.replication_stream_queue.pop_front();
      continue;
    }
    if (connection.replication_stream_generator == nullptr) break;
    auto next = connection.replication_stream_generator->Next();
    if (!next.ok()) {
      connection.replication_stream_failed = true;
      connection.replication_stream_generator.reset();
      break;
    }
    if (!next.value().has_value()) {
      connection.replication_stream_generator.reset();
      break;
    }
    connection.replication_stream_queue.push_back(std::move(*next.value()));
  }
  if (connection.output.empty() && connection.replication_stream_generator == nullptr &&
      connection.replication_stream_queue.empty() &&
      connection.replication_stream_final_offset != 0) {
    connection.replication_sent_offset =
        std::max(connection.replication_sent_offset, connection.replication_stream_final_offset);
    connection.replication_next_offset = connection.replication_sent_offset + 1U;
  }
}

bool QueueReplicationTransfer(Connection& connection, ReplicationChunkGenerator generator,
                              std::size_t budget, std::uint64_t final_offset) {
  if (connection.replication_stream_generator != nullptr ||
      !connection.replication_stream_queue.empty() ||
      connection.replication_stream_final_offset != 0)
    return false;
  connection.replication_stream_budget = budget;
  connection.replication_stream_final_offset = final_offset;
  connection.replication_stream_generator =
      std::make_unique<ReplicationChunkGenerator>(std::move(generator));
  return true;
}

bool QueueReplicationFrames(Connection& connection, std::vector<std::string> frames,
                            std::size_t budget) {
  std::size_t bytes = 0;
  for (const auto& frame : frames) {
    if (frame.size() > std::numeric_limits<std::size_t>::max() - bytes) return false;
    bytes += frame.size();
  }
  if (connection.output.size() > budget || connection.replication_queued_bytes > budget ||
      connection.output.size() + connection.replication_queued_bytes > budget ||
      bytes > budget - (connection.output.size() + connection.replication_queued_bytes))
    return false;
  for (auto& frame : frames) connection.replication_output_queue.push_back(std::move(frame));
  connection.replication_queued_bytes += bytes;
  return true;
}

bool QueueReplicationFrame(Connection& connection, std::string frame, std::size_t budget) {
  std::vector<std::string> frames;
  frames.push_back(std::move(frame));
  return QueueReplicationFrames(connection, std::move(frames), budget);
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
                         Dispatcher& dispatcher, std::string replication_role,
                         std::string replication_node_id, std::string replication_upstream,
                         ReplicationBacklog* replication_backlog,
                         std::uint64_t replication_heartbeat_ms, std::size_t max_events,
                         IReplicationExecutor* replication_executor, bool allow_peer_cycles)
    : server_config_(config),
      protocol_config_(protocol),
      dispatcher_(dispatcher),
      replication_role_(std::move(replication_role)),
      replication_node_id_(std::move(replication_node_id)),
      replication_upstream_(std::move(replication_upstream)),
      replication_heartbeat_interval_(std::chrono::milliseconds(replication_heartbeat_ms)),
      replication_backlog_(replication_backlog),
      replication_executor_(replication_executor),
      allow_peer_cycles_(allow_peer_cycles),
      max_events_(max_events) {}

EpollServer::EpollServer(const ServerConfig& config, const ProtocolConfig& protocol,
                         Dispatcher& dispatcher, std::size_t max_events)
    : EpollServer(config, protocol, dispatcher, "primary", {}, {}, nullptr, 1000, max_events,
                  nullptr) {}

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
  auto connect_upstream = [&]() {
    if (replication_upstream_.empty()) return false;
    const auto separator = replication_upstream_.rfind(':');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1U >= replication_upstream_.size())
      return false;
    const auto host = replication_upstream_.substr(0, separator);
    const auto port_text = replication_upstream_.substr(separator + 1U);
    std::uint32_t port = 0;
    const auto parsed =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() ||
        port == 0 || port > std::numeric_limits<std::uint16_t>::max())
      return false;
    const int upstream_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (upstream_fd < 0) return false;
    sockaddr_in upstream_address{};
    upstream_address.sin_family = AF_INET;
    upstream_address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &upstream_address.sin_addr) != 1) {
      ::close(upstream_fd);
      return false;
    }
    if (::connect(upstream_fd, reinterpret_cast<sockaddr*>(&upstream_address),
                  sizeof(upstream_address)) < 0 &&
        errno != EINPROGRESS) {
      ::close(upstream_fd);
      return false;
    }
    auto connection = std::make_unique<Connection>(
        protocol_config_, server_config_.max_input_buffer_bytes, next_connection_id++);
    connection->mode = Connection::Mode::kReplication;
    connection->upstream = true;
    connection->replication = std::make_unique<ReplicationConnection>(
        connection->last_activity, std::chrono::milliseconds(server_config_.handshake_timeout_ms),
        replication_role_);
    const auto hello =
        ReplicationHandshake::Encode({replication_node_id_, replication_role_,
                                      "snapshot,backlog,ack,heartbeat,full-sync-request"});
    if (!hello.ok() || !QueueReplicationFrame(*connection, std::move(hello).value(),
                                              server_config_.max_output_buffer_bytes)) {
      ::close(upstream_fd);
      return false;
    }
    PumpReplicationOutput(*connection, server_config_.max_output_buffer_bytes);
    connections.emplace(upstream_fd, std::move(connection));
    epoll_event upstream_event{.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET,
                               .data = {.fd = upstream_fd}};
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, upstream_fd, &upstream_event) < 0) {
      ::close(upstream_fd);
      connections.erase(upstream_fd);
      return false;
    }
    return true;
  };
  auto next_upstream_attempt = std::chrono::steady_clock::now();
  const auto replication_chunk_bytes = [&] {
    const auto budget = server_config_.max_output_buffer_bytes > kReplicationControlReserve
                            ? server_config_.max_output_buffer_bytes - kReplicationControlReserve
                            : server_config_.max_output_buffer_bytes;
    constexpr std::size_t kWireOverhead = 13U + 25U;
    return budget > kWireOverhead ? std::min<std::size_t>(64U * 1024U, budget - kWireOverhead) : 0U;
  };
  const auto queue_full_snapshot = [&](Connection& connection) {
    if (replication_chunk_bytes() == 0) return false;
    const auto view = dispatcher_.SnapshotView(ReplicationFrameCodec::kMaxTransferBytes);
    if (!view.ok()) return false;
    const auto final_offset = view.value().offset;
    const auto final_event_id = view.value().event_id;
    auto snapshot = ReplicationChunkGenerator::Snapshot(
        {final_offset, final_event_id}, std::move(view.value().entries),
        connection.replication_transfer_id++, replication_chunk_bytes());
    if (!snapshot.ok()) return false;
    if (!connection.output.empty()) return false;
    connection.replication_output_queue.clear();
    connection.replication_queued_bytes = 0;
    connection.replication_stream_queue.clear();
    const auto data_budget =
        server_config_.max_output_buffer_bytes > kReplicationControlReserve
            ? server_config_.max_output_buffer_bytes - kReplicationControlReserve
            : server_config_.max_output_buffer_bytes;
    if (!QueueReplicationTransfer(connection, std::move(snapshot).value(), data_budget,
                                  final_offset))
      return false;
    PumpReplicationOutput(connection, server_config_.max_output_buffer_bytes);
    connection.replication_snapshot_offset = final_offset;
    connection.replication_snapshot_event_id = final_event_id;
    return !connection.replication_stream_failed;
  };
  std::vector<epoll_event> events(max_events_);
  if (events.empty()) return {StatusCode::kInvalidArgument, "epoll event limit"};
  const auto shutdown_started = std::chrono::steady_clock::time_point{};
  auto shutdown_deadline = shutdown_started;
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    const bool has_upstream = std::any_of(connections.begin(), connections.end(),
                                          [](const auto& item) { return item.second->upstream; });
    if (!stopping_.load(std::memory_order_acquire) && !has_upstream &&
        now >= next_upstream_attempt) {
      static_cast<void>(connect_upstream());
      next_upstream_attempt = now + std::chrono::milliseconds(250);
    }
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
    if (replication_backlog_ != nullptr) {
      for (const auto& [fd, connection] : connections) {
        if (connection->upstream || connection->mode != Connection::Mode::kReplication ||
            !connection->replication ||
            connection->replication->state() != ReplicationConnectionState::kOnline ||
            connection->output.size() >= server_config_.output_high_watermark_bytes)
          continue;
        if (connection->replication_stream_final_offset != 0) continue;
        auto event = replication_backlog_->ReadOne(connection->replication_ack_offset + 1U);
        if (!event.ok() && event.status().code() == StatusCode::kNotFound) continue;
        if (!event.ok()) {
          if (!queue_full_snapshot(*connection)) connection->peer_closed = true;
          continue;
        }
        auto incremental = IncrementalEvents({std::move(event).value()});
        const auto final_offset = incremental.back().offset;
        auto encoded = ReplicationChunkGenerator::Events(std::move(incremental),
                                                         connection->replication_transfer_id++,
                                                         replication_chunk_bytes());
        if (!encoded.ok()) continue;
        if (!QueueReplicationTransfer(
                *connection, std::move(encoded).value(),
                server_config_.max_output_buffer_bytes > kReplicationControlReserve
                    ? server_config_.max_output_buffer_bytes - kReplicationControlReserve
                    : server_config_.max_output_buffer_bytes,
                final_offset))
          continue;
        PumpReplicationOutput(*connection, server_config_.max_output_buffer_bytes);
        epoll_event update{.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET, .data = {.fd = fd}};
        static_cast<void>(::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &update));
      }
    }
    for (const auto& [fd, connection] : connections) {
      if (connection->mode != Connection::Mode::kReplication || !connection->replication ||
          connection->replication->state() != ReplicationConnectionState::kOnline ||
          std::chrono::steady_clock::now() - connection->last_replication_heartbeat <
              replication_heartbeat_interval_)
        continue;
      const auto heartbeat =
          ReplicationFrameCodec::EncodeHeartbeat(connection->replication_ack_offset);
      if (!heartbeat.ok() || !QueueReplicationFrame(*connection, std::move(heartbeat).value(),
                                                    server_config_.max_output_buffer_bytes))
        continue;
      PumpReplicationOutput(*connection, server_config_.max_output_buffer_bytes);
      connection->last_replication_heartbeat = std::chrono::steady_clock::now();
      epoll_event update{.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET, .data = {.fd = fd}};
      static_cast<void>(::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &update));
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
      const bool replication_handshake =
          connection->mode == Connection::Mode::kReplication &&
          connection->replication != nullptr &&
          connection->replication->state() == ReplicationConnectionState::kHandshake;
      const auto limit = connection->mode == Connection::Mode::kUnknown || replication_handshake
                             ? server_config_.handshake_timeout_ms
                             : (connection->waiting_for_frame ? protocol_config_.parse_timeout_ms
                                                              : server_config_.idle_timeout_ms);
      if (elapsed.count() > static_cast<std::int64_t>(limit)) expired.push_back(fd);
      if (connection->mode == Connection::Mode::kReplication && connection->replication &&
          connection->replication->state() == ReplicationConnectionState::kOnline &&
          scan_time - connection->last_peer_heartbeat > replication_heartbeat_interval_ * 3)
        expired.push_back(fd);
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
            if (connection->probe.starts_with("KVRP")) {
              connection->mode = Connection::Mode::kReplication;
              connection->replication = std::make_unique<ReplicationConnection>(
                  connection->last_activity,
                  std::chrono::milliseconds(server_config_.handshake_timeout_ms),
                  allow_peer_cycles_ ? "dual" : "primary");
            } else if (connection->probe.front() == '*') {
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
          if (connection->mode == Connection::Mode::kReplication) {
            if (!connection->replication->peer().has_value()) {
              const auto status = connection->replication->Receive(
                  received_bytes, std::chrono::steady_clock::now());
              if (!status.ok()) {
                connection->peer_closed = true;
                break;
              }
            } else {
              if (received_bytes.size() > server_config_.max_input_buffer_bytes -
                                              std::min(server_config_.max_input_buffer_bytes,
                                                       connection->replication_input.size())) {
                connection->peer_closed = true;
                break;
              }
              connection->replication_input.append(received_bytes);
            }
            if (connection->replication->state() == ReplicationConnectionState::kHandshake &&
                connection->replication->peer().has_value()) {
              if (!connection->upstream)
                connection->upstream = connection->replication->peer()->role == "primary";
              Status transition;
              if (connection->upstream) {
                connection->replica_applier =
                    std::make_unique<ReplicaSyncApplier>(dispatcher_.engine());
                transition = connection->replication->BeginFullSync();
              } else {
                const auto hello = ReplicationHandshake::Encode(
                    {replication_node_id_, "primary",
                     "snapshot,backlog,ack,heartbeat,full-sync-request"});
                if (!hello.ok() || !QueueReplicationFrame(*connection, std::move(hello).value(),
                                                          server_config_.max_output_buffer_bytes)) {
                  connection->peer_closed = true;
                  break;
                }
                PumpReplicationOutput(*connection, server_config_.max_output_buffer_bytes);
                if (replication_backlog_ == nullptr) {
                  connection->peer_closed = true;
                  break;
                }
                const auto snapshot_view =
                    dispatcher_.SnapshotView(ReplicationFrameCodec::kMaxTransferBytes);
                if (!snapshot_view.ok()) {
                  connection->peer_closed = true;
                  break;
                }
                const auto final_offset = snapshot_view.value().offset;
                const auto final_event_id = snapshot_view.value().event_id;
                if (replication_chunk_bytes() == 0) {
                  connection->peer_closed = true;
                  break;
                }
                auto snapshot = ReplicationChunkGenerator::Snapshot(
                    {final_offset, final_event_id}, std::move(snapshot_view.value().entries),
                    connection->replication_transfer_id++, replication_chunk_bytes());
                if (!snapshot.ok()) {
                  connection->peer_closed = true;
                  break;
                }
                if (!QueueReplicationTransfer(
                        *connection, std::move(snapshot).value(),
                        server_config_.max_output_buffer_bytes > kReplicationControlReserve
                            ? server_config_.max_output_buffer_bytes - kReplicationControlReserve
                            : server_config_.max_output_buffer_bytes,
                        final_offset)) {
                  connection->peer_closed = true;
                  break;
                }
                PumpReplicationOutput(*connection, server_config_.max_output_buffer_bytes);
                connection->replication_snapshot_offset = final_offset;
                connection->replication_snapshot_event_id = final_event_id;
                transition = connection->replication->MarkOnline();
              }
              if (!transition.ok()) {
                connection->peer_closed = true;
                break;
              }
            }
            const auto pending = connection->replication->TakePendingBytes();
            if (pending.size() > server_config_.max_input_buffer_bytes -
                                     std::min(server_config_.max_input_buffer_bytes,
                                              connection->replication_input.size())) {
              connection->peer_closed = true;
              break;
            }
            connection->replication_input.append(pending);
            while (!connection->replication_input.empty()) {
              auto frame = ReplicationFrameCodec::Decode(connection->replication_input);
              if (!frame.ok()) {
                connection->peer_closed = true;
                break;
              }
              if (!frame.value().has_value()) break;
              if (frame.value()->type == ReplicationFrameType::kChunk) {
                const auto& chunk = *frame.value();
                if (connection->replication_chunks.empty() ||
                    connection->replication_chunks.front().transfer_id == chunk.transfer_id) {
                  if (connection->replication_chunks.empty())
                    connection->replication_chunks.reserve(chunk.chunk_count);
                  connection->replication_chunk_bytes += chunk.payload.size();
                  if (connection->replication_chunk_bytes >
                          ReplicationFrameCodec::kMaxTransferBytes ||
                      connection->replication_chunks.size() >= 16'384U) {
                    connection->peer_closed = true;
                    break;
                  }
                  connection->replication_chunks.push_back(chunk);
                  if (connection->replication_chunks.size() < chunk.chunk_count) continue;
                  auto payload =
                      ReplicationFrameCodec::ReassembleChunks(connection->replication_chunks);
                  if (!payload.ok()) {
                    connection->peer_closed = true;
                    break;
                  }
                  frame = Result<std::optional<ReplicationFrame>>(std::optional<ReplicationFrame>(
                      ReplicationFrame{chunk.chunk_type, std::move(payload).value()}));
                  connection->replication_chunks.clear();
                  connection->replication_chunk_bytes = 0;
                } else {
                  connection->peer_closed = true;
                  break;
                }
              }
              if (!connection->upstream && frame.value()->type == ReplicationFrameType::kAck) {
                const auto ack = ReplicationFrameCodec::DecodeAck(*frame.value());
                if (!ack.ok() || ack.value().offset == std::numeric_limits<std::uint64_t>::max() ||
                    ack.value().offset > connection->replication_sent_offset ||
                    replication_backlog_ == nullptr) {
                  connection->peer_closed = true;
                  break;
                }
                if (ack.value().offset < connection->replication_ack_offset ||
                    (ack.value().offset == 0 && ack.value().event_id != 0)) {
                  connection->peer_closed = true;
                  break;
                }
                const bool snapshot_ack =
                    ack.value().offset == connection->replication_snapshot_offset &&
                    ack.value().event_id == connection->replication_snapshot_event_id;
                if (ack.value().offset != 0 && !snapshot_ack) {
                  const auto acknowledged = replication_backlog_->ReadOne(ack.value().offset);
                  if (!acknowledged.ok() || acknowledged.value().event_id != ack.value().event_id) {
                    connection->peer_closed = true;
                    break;
                  }
                }
                connection->replication_ack_offset = ack.value().offset;
                if (ack.value().offset == connection->replication_stream_final_offset)
                  connection->replication_stream_final_offset = 0;
              } else if (!connection->upstream &&
                         frame.value()->type == ReplicationFrameType::kHeartbeat) {
                const auto received_heartbeat =
                    ReplicationFrameCodec::DecodeHeartbeat(*frame.value());
                if (!received_heartbeat.ok()) {
                  connection->peer_closed = true;
                  break;
                }
                connection->last_peer_heartbeat = std::chrono::steady_clock::now();
                const auto heartbeat_frame = ReplicationFrameCodec::EncodeHeartbeat(
                    connection->replication_next_offset - 1U);
                if (!heartbeat_frame.ok()) {
                  connection->peer_closed = true;
                  break;
                }
                if (heartbeat_frame.value().size() >
                    server_config_.max_output_buffer_bytes -
                        std::min(server_config_.max_output_buffer_bytes,
                                 connection->output.size())) {
                  connection->peer_closed = true;
                  break;
                }
                if (!QueueReplicationFrame(*connection, std::move(heartbeat_frame).value(),
                                           server_config_.max_output_buffer_bytes))
                  continue;
              } else if (!connection->upstream &&
                         frame.value()->type == ReplicationFrameType::kFullSyncRequest) {
                if (!ReplicationFrameCodec::DecodeFullSyncRequest(*frame.value()).ok()) {
                  connection->peer_closed = true;
                  break;
                }
                const auto view =
                    dispatcher_.SnapshotView(ReplicationFrameCodec::kMaxTransferBytes);
                if (!view.ok()) {
                  connection->peer_closed = true;
                  break;
                }
                const auto final_offset = view.value().offset;
                const auto final_event_id = view.value().event_id;
                if (replication_chunk_bytes() == 0) {
                  connection->peer_closed = true;
                  break;
                }
                auto snapshot = ReplicationChunkGenerator::Snapshot(
                    {final_offset, final_event_id}, std::move(view.value().entries),
                    connection->replication_transfer_id++, replication_chunk_bytes());
                if (!snapshot.ok()) {
                  connection->peer_closed = true;
                  break;
                }
                if (!connection->output.empty() ||
                    connection->replication_stream_generator != nullptr ||
                    !connection->replication_stream_queue.empty()) {
                  connection->peer_closed = true;
                  break;
                }
                connection->replication_output_queue.clear();
                connection->replication_queued_bytes = 0;
                connection->replication_stream_queue.clear();
                connection->replication_stream_generator.reset();
                connection->replication_stream_final_offset = 0;
                if (!QueueReplicationTransfer(
                        *connection, std::move(snapshot).value(),
                        server_config_.max_output_buffer_bytes > kReplicationControlReserve
                            ? server_config_.max_output_buffer_bytes - kReplicationControlReserve
                            : server_config_.max_output_buffer_bytes,
                        final_offset)) {
                  connection->peer_closed = true;
                  break;
                }
                PumpReplicationOutput(*connection, server_config_.max_output_buffer_bytes);
                connection->replication_snapshot_offset = final_offset;
                connection->replication_snapshot_event_id = final_event_id;
              } else if (connection->upstream &&
                         frame.value()->type == ReplicationFrameType::kSnapshot) {
                const auto snapshot = ReplicationFrameCodec::DecodeSnapshot(*frame.value());
                const auto current_offset = dispatcher_.next_offset() - 1U;
                const auto current_event_id = dispatcher_.next_event_id() - 1U;
                const bool stale_snapshot =
                    snapshot.ok() &&
                    (snapshot.value().first.offset < current_offset ||
                     (snapshot.value().first.offset == current_offset &&
                      snapshot.value().first.event_id < current_event_id));
                const auto install_status =
                    snapshot.ok() && connection->replica_applier
                        ? (stale_snapshot
                               ? Status::Ok()
                               : ExecuteReplicationTask(
                                     replication_executor_,
                                     [&] {
                                       return connection->replica_applier->InstallFullSync(
                                           snapshot.value().second, snapshot.value().first);
                                     }))
                        : Status{StatusCode::kCorruption, "snapshot decode or applier failed"};
                if (!install_status.ok()) {
                  connection->peer_closed = true;
                  break;
                }
                if (!stale_snapshot)
                  dispatcher_.SetReplicationPoint(snapshot.value().first.offset,
                                                  snapshot.value().first.event_id);
                if (replication_backlog_ != nullptr && !stale_snapshot) {
                  replication_backlog_->Reset();
                  for (auto& [peer_fd, downstream] : connections) {
                    if (peer_fd != fd && !downstream->upstream &&
                        downstream->mode == Connection::Mode::kReplication)
                      AbortReplicationTransfer(*downstream);
                  }
                }
                const auto ack_point = stale_snapshot && connection->replica_applier
                                           ? connection->replica_applier->applied()
                                           : ReplicationAck{snapshot.value().first.offset,
                                                            snapshot.value().first.event_id};
                connection->replication_ack_offset = ack_point.offset;
                connection->replication_next_offset = ack_point.offset + 1U;
                const auto snapshot_ack = ReplicationFrameCodec::EncodeAck(ack_point);
                if (!snapshot_ack.ok() || snapshot_ack.value().size() >
                                              server_config_.max_output_buffer_bytes -
                                                  std::min(server_config_.max_output_buffer_bytes,
                                                           connection->output.size())) {
                  connection->peer_closed = true;
                  break;
                }
                if (!QueueReplicationFrame(*connection, std::move(snapshot_ack).value(),
                                           server_config_.max_output_buffer_bytes)) {
                  connection->peer_closed = true;
                  break;
                }
                if (connection->replication->state() == ReplicationConnectionState::kFullSync &&
                    !connection->replication->BeginCatchUp().ok()) {
                  connection->peer_closed = true;
                  break;
                }
                if (connection->replication->state() == ReplicationConnectionState::kCatchUp)
                  static_cast<void>(connection->replication->MarkOnline());
              } else if (connection->upstream &&
                         frame.value()->type == ReplicationFrameType::kEvents) {
                auto events = ReplicationFrameCodec::DecodeEvents(*frame.value());
                std::vector<WriteEvent> unseen_events;
                if (events.ok()) {
                  unseen_events.reserve(events.value().size());
                  for (const auto& event : events.value()) {
                    if (event.origin_node.empty()) {
                      unseen_events.push_back(event);
                      continue;
                    }
                    const auto observation =
                        live_sync_deduplicator_.Observe(event, LiveSyncDeduplicator::Clock::now());
                    if (observation == LiveSyncDeduplicator::Observation::kConflict) {
                      events = Status{StatusCode::kCorruption,
                                      "LiveSync identity has conflicting content"};
                      break;
                    }
                    if (observation == LiveSyncDeduplicator::Observation::kNew)
                      unseen_events.push_back(event);
                  }
                }
                const auto apply_status =
                    events.ok() && connection->replica_applier
                        ? ExecuteReplicationTask(
                              replication_executor_,
                              [&] {
                                return connection->replica_applier->ApplyIncremental(
                                    events.value());
                              })
                        : Status{StatusCode::kCorruption, "event decode or applier failed"};
                if (!apply_status.ok()) {
                  for (const auto& event : unseen_events) live_sync_deduplicator_.Forget(event);
                  const auto request = ReplicationFrameCodec::EncodeFullSyncRequest(
                      connection->replication_next_offset);
                  if (!request.ok() ||
                      request.value().size() > server_config_.max_output_buffer_bytes -
                                                   std::min(server_config_.max_output_buffer_bytes,
                                                            connection->output.size())) {
                    connection->peer_closed = true;
                    break;
                  }
                  if (!QueueReplicationFrame(*connection, std::move(request).value(),
                                             server_config_.max_output_buffer_bytes)) {
                    connection->peer_closed = true;
                    break;
                  }
                  if (!connection->replication->BeginFullSync().ok()) {
                    connection->peer_closed = true;
                    break;
                  }
                  continue;
                }
                const auto applied = connection->replica_applier->applied();
                dispatcher_.SetReplicationPoint(applied.offset, applied.event_id);
                if (replication_backlog_ != nullptr && !unseen_events.empty()) {
                  auto forwarded = unseen_events;
                  for (auto& event : forwarded) {
                    event.source = WriteSource::kClient;
                    event.checksum = event.ComputeChecksum();
                  }
                  const auto publish = replication_backlog_->Append(forwarded);
                  if (!publish.ok()) {
                    for (const auto& event : unseen_events) live_sync_deduplicator_.Forget(event);
                    replication_backlog_->Reset();
                    for (auto& [peer_fd, downstream] : connections) {
                      if (peer_fd != fd && !downstream->upstream &&
                          downstream->mode == Connection::Mode::kReplication)
                        AbortReplicationTransfer(*downstream);
                    }
                    connection->peer_closed = true;
                    break;
                  }
                }
                const auto ack = ReplicationFrameCodec::EncodeAck(applied);
                if (!ack.ok()) {
                  connection->peer_closed = true;
                  break;
                }
                if (!QueueReplicationFrame(*connection, std::move(ack).value(),
                                           server_config_.max_output_buffer_bytes)) {
                  connection->peer_closed = true;
                  break;
                }
                PumpReplicationOutput(*connection, server_config_.max_output_buffer_bytes);
                connection->replication_ack_offset = applied.offset;
                connection->replication_next_offset = applied.offset + 1U;
                if (connection->replication->state() == ReplicationConnectionState::kCatchUp)
                  static_cast<void>(connection->replication->MarkOnline());
              } else if (connection->upstream &&
                         frame.value()->type == ReplicationFrameType::kHeartbeat) {
                if (!ReplicationFrameCodec::DecodeHeartbeat(*frame.value()).ok()) {
                  connection->peer_closed = true;
                  break;
                }
                connection->last_peer_heartbeat = std::chrono::steady_clock::now();
              } else {
                connection->peer_closed = true;
                break;
              }
            }
            if (!connection->upstream && replication_backlog_ != nullptr &&
                connection->replication->state() == ReplicationConnectionState::kOnline) {
              auto event = replication_backlog_->ReadOne(connection->replication_ack_offset + 1U);
              if (!event.ok() && event.status().code() == StatusCode::kNotFound) continue;
              if (!event.ok()) {
                const auto view =
                    dispatcher_.SnapshotView(ReplicationFrameCodec::kMaxTransferBytes);
                if (!view.ok()) {
                  connection->peer_closed = true;
                  break;
                }
                const auto final_offset = view.value().offset;
                const auto final_event_id = view.value().event_id;
                if (replication_chunk_bytes() == 0) {
                  connection->peer_closed = true;
                  break;
                }
                auto snapshot = ReplicationChunkGenerator::Snapshot(
                    {final_offset, final_event_id}, std::move(view.value().entries),
                    connection->replication_transfer_id++, replication_chunk_bytes());
                if (!snapshot.ok()) {
                  connection->peer_closed = true;
                  break;
                }
                if (!connection->output.empty() ||
                    connection->replication_stream_generator != nullptr ||
                    !connection->replication_stream_queue.empty()) {
                  connection->peer_closed = true;
                  break;
                }
                connection->replication_output_queue.clear();
                connection->replication_queued_bytes = 0;
                connection->replication_stream_queue.clear();
                connection->replication_stream_generator.reset();
                connection->replication_stream_final_offset = 0;
                if (!QueueReplicationTransfer(
                        *connection, std::move(snapshot).value(),
                        server_config_.max_output_buffer_bytes > kReplicationControlReserve
                            ? server_config_.max_output_buffer_bytes - kReplicationControlReserve
                            : server_config_.max_output_buffer_bytes,
                        final_offset)) {
                  connection->peer_closed = true;
                  break;
                }
                PumpReplicationOutput(*connection, server_config_.max_output_buffer_bytes);
                connection->replication_snapshot_offset = final_offset;
                connection->replication_snapshot_event_id = final_event_id;
                continue;
              }
              if (event.ok()) {
                auto incremental = IncrementalEvents({std::move(event).value()});
                const auto final_offset = incremental.back().offset;
                auto encoded = ReplicationChunkGenerator::Events(
                    std::move(incremental), connection->replication_transfer_id++,
                    replication_chunk_bytes());
                if (!encoded.ok()) {
                  connection->peer_closed = true;
                  break;
                }
                if (!QueueReplicationTransfer(
                        *connection, std::move(encoded).value(),
                        server_config_.max_output_buffer_bytes > kReplicationControlReserve
                            ? server_config_.max_output_buffer_bytes - kReplicationControlReserve
                            : server_config_.max_output_buffer_bytes,
                        final_offset)) {
                  connection->peer_closed = true;
                  break;
                }
                PumpReplicationOutput(*connection, server_config_.max_output_buffer_bytes);
              }
            }
            if (connection->peer_closed) break;
            break;
          }
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
        if (connection->output.empty() && connection->replication_stream_generator == nullptr &&
            connection->replication_stream_queue.empty() &&
            connection->replication_stream_final_offset != 0) {
          connection->replication_sent_offset = std::max(
              connection->replication_sent_offset, connection->replication_stream_final_offset);
          connection->replication_next_offset = connection->replication_sent_offset + 1U;
        }
      }
      if (connections.contains(fd)) {
        if (!connection->peer_closed)
          PumpReplicationOutput(*connection, server_config_.max_output_buffer_bytes);
        if (connection->output.empty() && connection->replication_stream_generator == nullptr &&
            connection->replication_stream_queue.empty() &&
            connection->replication_stream_final_offset != 0) {
          connection->replication_sent_offset = std::max(
              connection->replication_sent_offset, connection->replication_stream_final_offset);
          connection->replication_next_offset = connection->replication_sent_offset + 1U;
        }
        if (connection->output.size() < server_config_.output_high_watermark_bytes &&
            (!connection->pending_commands.empty() || !connection->pending_batches.empty()))
          DrainPending(*connection, dispatcher_, server_config_, protocol_config_);
        const bool has_pending =
            !connection->pending_commands.empty() || !connection->pending_batches.empty();
        if ((connection->peer_closed || stopping_.load(std::memory_order_acquire)) &&
            connection->output.empty() && connection->replication_output_queue.empty() &&
            connection->replication_stream_queue.empty() &&
            connection->replication_stream_generator == nullptr && !has_pending) {
          CloseConnection(fd, epoll_fd_, connections);
          continue;
        }
        const bool read_paused =
            connection->output.size() >= server_config_.output_high_watermark_bytes || has_pending;
        const bool has_replication_output = !connection->replication_output_queue.empty() ||
                                            !connection->replication_stream_queue.empty();
        epoll_event update{.events = static_cast<std::uint32_t>(EPOLLRDHUP | EPOLLET) |
                                     (!read_paused && !connection->peer_closed
                                          ? static_cast<std::uint32_t>(EPOLLIN)
                                          : 0U) |
                                     (!connection->output.empty() || has_replication_output
                                          ? static_cast<std::uint32_t>(EPOLLOUT)
                                          : 0U),
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
