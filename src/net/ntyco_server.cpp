#include "kvstore/net/ntyco_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>

#include "kvstore/protocol/batch.hpp"
#include "kvstore/protocol/native.hpp"
#include "kvstore/protocol/resp.hpp"

#ifdef KVSTORE_HAVE_NTYCO
extern "C" {
int kvstore_ntyco_init(unsigned long stack_bytes);
int kvstore_ntyco_spawn(void (*callback)(void *), void *argument);
void kvstore_ntyco_run(void);
void kvstore_ntyco_set_timeout(unsigned long usecs);
int kvstore_ntyco_socket(int domain, int type, int protocol);
int kvstore_ntyco_accept(int fd, struct sockaddr *address, socklen_t *length);
int kvstore_ntyco_accept_timed(int fd, struct sockaddr *address, socklen_t *length,
                               unsigned long timeout_ms);
ssize_t kvstore_ntyco_recv(int fd, void *buffer, size_t length, int flags);
ssize_t kvstore_ntyco_recv_timed(int fd, void *buffer, size_t length, int flags,
                                 unsigned long timeout_ms);
ssize_t kvstore_ntyco_send(int fd, const void *buffer, size_t length, int flags);
ssize_t kvstore_ntyco_send_timed(int fd, const void *buffer, size_t length, int flags,
                                 unsigned long timeout_ms);
int kvstore_ntyco_close(int fd);
void kvstore_ntyco_wakeup(void);
void kvstore_ntyco_cancel_all(void);
}
#endif

namespace kvstore {
namespace {

struct Client {
  NtycoServer *server;
  int fd;
  std::string input;
  std::string output;
  std::string probe;
  std::deque<Command> pending_commands;
  std::deque<std::vector<Command>> pending_frames;
  std::deque<bool> pending_frame_reject;
  bool pending_reject{false};
  bool pending_budget_exhausted{false};
  RespParser resp;
  NativeParser native;
  BatchParser batch;
  std::uint64_t request_id{1};
  std::uint64_t connection_id{};
  bool registered{false};

  Client(NtycoServer *owner, int socket, const ProtocolConfig& config, std::size_t id)
      : server(owner),
        fd(socket),
        resp(config.max_frame_bytes),
        native(config.max_key_bytes, config.max_value_bytes, config.max_frame_bytes),
        batch(config.max_batch_records, config.max_frame_bytes, config.max_frame_bytes,
              config.max_key_bytes, config.max_value_bytes),
        connection_id(id) {}
};

bool Enabled(const ProtocolConfig& config, std::string_view name) {
  return std::find(config.enabled.begin(), config.enabled.end(), name) != config.enabled.end();
}

void Close(Client& client) {
#ifdef KVSTORE_HAVE_NTYCO
  if (client.fd >= 0) kvstore_ntyco_close(client.fd);
#else
  static_cast<void>(client);
#endif
  client.fd = -1;
}

Status FeedAndServe(Client& client, std::string_view bytes) {
  const auto input_limit = client.server->server_config().max_input_buffer_bytes;
  const auto parser_buffered = client.resp.buffered_bytes() + client.native.buffered_bytes() +
                               client.batch.buffered_bytes();
  const auto buffered_before = client.input.size() +
                                std::min(input_limit, parser_buffered);
  if (bytes.size() > input_limit - std::min(input_limit, buffered_before))
    return {StatusCode::kLimitExceeded, "input buffer limit exceeded"};
  client.input.append(bytes);
  if (client.probe.empty()) {
    if (client.input.starts_with('*')) {
      client.probe = "*";
      if (!Enabled(client.server->protocol_config(), "resp2"))
        return {StatusCode::kUnsupported, "RESP protocol is disabled"};
    } else if (client.input.size() < 4 && std::string_view("KV/1").starts_with(client.input)) {
      return Status::Ok();
    } else if (client.input.size() < 4 && std::string_view("KVB1").starts_with(client.input)) {
      return Status::Ok();
    } else if (client.input.rfind("KV/1", 0) == 0) {
      client.probe = "KV/1";
      if (!Enabled(client.server->protocol_config(), "text-kv"))
        return {StatusCode::kUnsupported, "text-kv protocol is disabled"};
    } else if (client.input.rfind("KVB1", 0) == 0) {
      client.probe = "KVB1";
      if (!Enabled(client.server->protocol_config(), "batch-v1"))
        return {StatusCode::kUnsupported, "batch-v1 protocol is disabled"};
    } else {
      return {StatusCode::kInvalidArgument, "unknown protocol"};
    }
  }
  const auto feed = client.input;
  client.input.clear();
  const auto process_unlocked = [&client](Command command, std::size_t response_budget) {
    RequestContext context;
    context.request_id = client.request_id++;
    context.connection_id = client.connection_id;
    context.deadline = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(client.server->server_config().idle_timeout_ms);
    context.response_budget = response_budget;
    return client.server->dispatcher().Execute(command, context);
  };
  const auto process = [&client, &process_unlocked](Command command, std::size_t response_budget) {
    std::scoped_lock lock(client.server->execution_mutex());
    if (client.server->stopping()) return CommandResponse{false, {}, "CANCELLED server is stopping",
                                                           false, false, {}, false};
    return process_unlocked(std::move(command), response_budget);
  };
  const auto BudgetResponse = [] {
    return CommandResponse{false, {}, "BUSY output buffer limit exceeded", false, false, {}, false};
  };
  constexpr std::size_t kResponseReserve = 128U;
  // Reserve the complete encoded frame, including every record header, before dispatching.
  // The response budget below is derived from the same bound, so commit cannot overflow it.
  const auto reserve_batch_output = [&client](const auto& commands) {
    const auto max_output = client.server->server_config().max_output_buffer_bytes;
    const auto max_value = client.server->protocol_config().max_value_bytes;
    constexpr std::size_t kBatchHeader = 20U;
    constexpr std::size_t kRecordHeader = 10U;
    constexpr std::size_t kErrorAllowance = 128U;
    if (max_value > std::numeric_limits<std::size_t>::max() - kErrorAllowance - kRecordHeader ||
        commands.size() >
            (std::numeric_limits<std::size_t>::max() - kBatchHeader) /
                (kRecordHeader + max_value + kErrorAllowance))
      return false;
    const auto reservation = kBatchHeader +
                             commands.size() * (kRecordHeader + max_value + kErrorAllowance);
    return client.output.size() <= max_output && reservation <= max_output - client.output.size();
  };
  if (!client.probe.empty() && client.probe[0] == '*') {
    auto fed = client.resp.Feed(feed);
    if (!fed.ok()) return fed;
    auto commands = client.resp.ParseAvailable();
    if (!commands.ok()) return commands.status();
    if (commands.value().size() + client.pending_commands.size() >
        client.server->server_config().max_inflight_requests) {
      for (std::size_t index = 0; index < commands.value().size(); ++index)
        client.output += EncodeResp(BudgetResponse());
      return Status::Ok();
    }
    const bool reject_batch = false;
    if (client.pending_commands.empty()) {
      client.pending_reject = reject_batch;
      client.pending_budget_exhausted = false;
    }
    for (auto& command : commands.value()) client.pending_commands.push_back(std::move(command));
    while (!client.pending_commands.empty()) {
      if (client.output.size() >= client.server->server_config().output_high_watermark_bytes &&
          !client.output.empty()) break;
      if (client.server->stopping()) return {StatusCode::kCancelled, "server is stopping"};
      auto command = std::move(client.pending_commands.front());
      client.pending_commands.pop_front();
      const auto remaining = client.server->server_config().max_output_buffer_bytes -
                             std::min(client.server->server_config().max_output_buffer_bytes,
                                      client.output.size());
      auto response = client.pending_reject || client.pending_budget_exhausted || remaining <= kResponseReserve
                          ? BudgetResponse()
                          : process(std::move(command), remaining - kResponseReserve);
      if (!response.ok && response.error.find("response exceeds connection limit") != std::string::npos) {
        response = BudgetResponse();
        client.pending_budget_exhausted = true;
      }
      auto encoded = EncodeResp(response);
      const auto max_output = client.server->server_config().max_output_buffer_bytes;
      if (encoded.size() > max_output - std::min(max_output, client.output.size())) {
        encoded = EncodeResp(BudgetResponse());
        client.pending_budget_exhausted = true;
      }
      client.output += std::move(encoded);
    }
    client.input.clear();
  } else if (client.probe.rfind("KV/1", 0) == 0) {
    auto fed = client.native.Feed(feed);
    if (!fed.ok()) return fed;
    auto commands = client.native.ParseAvailable();
    if (!commands.ok()) return commands.status();
    if (commands.value().size() + client.pending_commands.size() >
        client.server->server_config().max_inflight_requests) {
      for (std::size_t index = 0; index < commands.value().size(); ++index)
        client.output += EncodeNative(BudgetResponse());
      return Status::Ok();
    }
    const bool reject_batch = false;
    if (client.pending_commands.empty()) {
      client.pending_reject = reject_batch;
      client.pending_budget_exhausted = false;
    }
    for (auto& command : commands.value()) client.pending_commands.push_back(std::move(command));
    while (!client.pending_commands.empty()) {
      if (client.output.size() >= client.server->server_config().output_high_watermark_bytes &&
          !client.output.empty()) break;
      if (client.server->stopping()) return {StatusCode::kCancelled, "server is stopping"};
      auto command = std::move(client.pending_commands.front());
      client.pending_commands.pop_front();
      const auto remaining = client.server->server_config().max_output_buffer_bytes -
                             std::min(client.server->server_config().max_output_buffer_bytes,
                                      client.output.size());
      auto response = client.pending_reject || client.pending_budget_exhausted || remaining <= kResponseReserve
                          ? BudgetResponse()
                          : process(std::move(command), remaining - kResponseReserve);
      if (!response.ok && response.error.find("response exceeds connection limit") != std::string::npos) {
        response = BudgetResponse();
        client.pending_budget_exhausted = true;
      }
      auto encoded = EncodeNative(response);
      const auto max_output = client.server->server_config().max_output_buffer_bytes;
      if (encoded.size() > max_output - std::min(max_output, client.output.size())) {
        encoded = EncodeNative(BudgetResponse());
        client.pending_budget_exhausted = true;
      }
      client.output += std::move(encoded);
    }
    client.input.clear();
  } else if (client.probe.rfind("KVB1", 0) == 0) {
    auto fed = client.batch.Feed(feed);
    if (!fed.ok()) return fed;
    auto frames = client.batch.ParseAvailableFrames();
    if (!frames.ok()) return frames.status();
    std::size_t command_count = 0;
    for (const auto& frame : frames.value()) {
      if (frame.size() > client.server->server_config().max_inflight_requests -
                            std::min(client.server->server_config().max_inflight_requests,
                                     command_count))
        return {StatusCode::kBusy, "request queue limit exceeded"};
      command_count += frame.size();
    }
    std::size_t pending_frame_commands = 0;
    for (const auto& pending : client.pending_frames) {
      if (pending.size() > std::numeric_limits<std::size_t>::max() - pending_frame_commands)
        return {StatusCode::kBusy, "request queue limit exceeded"};
      pending_frame_commands += pending.size();
    }
    if (pending_frame_commands > std::numeric_limits<std::size_t>::max() - command_count ||
        command_count + pending_frame_commands >
        client.server->server_config().max_inflight_requests) {
      for (const auto& frame : frames.value()) {
        std::vector<CommandResponse> rejected(frame.size(), BudgetResponse());
        auto encoded = EncodeBatch(rejected);
        const auto max_output = client.server->server_config().max_output_buffer_bytes;
        if (encoded.size() > max_output - std::min(max_output, client.output.size()))
          return {StatusCode::kBusy, "request queue limit exceeded"};
        client.output += std::move(encoded);
      }
      return Status::Ok();
    }
    for (auto& frame : frames.value()) {
      const bool has_mutation = std::any_of(frame.begin(), frame.end(), [](const Command& command) {
        return command.type == CommandType::kSet || command.type == CommandType::kMod ||
               command.type == CommandType::kDel;
      });
      // A multi-record frame containing writes is rejected as one unit. This keeps the
      // reservation transactional until Dispatcher exposes a batch mutation API.
      if (has_mutation && frame.size() > 1U) {
        client.pending_frame_reject.push_back(true);
      } else {
        client.pending_frame_reject.push_back(!reserve_batch_output(frame));
      }
      client.pending_frames.push_back(std::move(frame));
    }
    while (!client.pending_frames.empty()) {
      if (client.output.size() >= client.server->server_config().output_high_watermark_bytes &&
          !client.output.empty()) break;
      if (client.server->stopping()) {
        for (const auto& pending : client.pending_frames)
          client.output += EncodeBatch(std::vector<CommandResponse>(pending.size(), BudgetResponse()));
        client.pending_frames.clear();
        client.pending_frame_reject.clear();
        return {StatusCode::kCancelled, "server is stopping"};
      }
      auto frame = std::move(client.pending_frames.front());
      client.pending_frames.pop_front();
      const bool queued_reject = client.pending_frame_reject.front();
      client.pending_frame_reject.pop_front();
      if (client.server->stopping()) return {StatusCode::kCancelled, "server is stopping"};
      std::scoped_lock frame_lock(client.server->execution_mutex());
      const bool reject_frame = queued_reject || !reserve_batch_output(frame);
      if (reject_frame) {
        std::vector<CommandResponse> rejected(frame.size(), BudgetResponse());
        auto encoded = EncodeBatch(rejected);
        const auto max_output = client.server->server_config().max_output_buffer_bytes;
        if (encoded.size() > max_output - std::min(max_output, client.output.size()))
          return {StatusCode::kBusy, "output buffer limit exceeded"};
        client.output += std::move(encoded);
        continue;
      }
      std::vector<CommandResponse> responses;
      std::size_t response_bytes = 0;
      bool cancelled = false;
      for (auto& command : frame) {
        const auto max_output = client.server->server_config().max_output_buffer_bytes;
        const auto remaining = client.output.size() >= max_output || response_bytes > max_output - client.output.size()
                                   ? std::size_t{0}
                                   : max_output - client.output.size() - response_bytes;
        auto response = reject_frame || remaining <= kResponseReserve
                            ? BudgetResponse()
                            : (cancelled || client.server->stopping()
                                   ? BudgetResponse()
                                   : process_unlocked(std::move(command), remaining - kResponseReserve));
        if (client.server->stopping()) cancelled = true;
        if (!response.ok && response.error.find("response exceeds connection limit") != std::string::npos) {
          response = BudgetResponse();
        }
        responses.push_back(std::move(response));
        const auto encoded_response = EncodeBatch({responses.back()});
        const auto response_size = encoded_response.size() > 20U
                                       ? encoded_response.size() - 20U
                                       : std::size_t{0};
        if (response_size > std::numeric_limits<std::size_t>::max() - response_bytes)
          return {StatusCode::kBusy, "output buffer limit exceeded"};
        response_bytes += response_size;
      }
       const auto encoded = EncodeBatch(responses);
       // The reservation is a proof obligation: every response is bounded by max_value + allowance.
       const auto max_output = client.server->server_config().max_output_buffer_bytes;
       if (encoded.size() > max_output - std::min(max_output, client.output.size()))
         return {StatusCode::kBusy, "output buffer limit exceeded"};
       client.output += encoded;
      if (cancelled) return {StatusCode::kCancelled, "server is stopping"};
    }
  }
  if (client.pending_commands.empty()) {
    client.pending_reject = false;
    client.pending_budget_exhausted = false;
  }
  const auto buffered = client.resp.buffered_bytes() + client.native.buffered_bytes() +
                        client.batch.buffered_bytes();
  if (buffered > client.server->server_config().max_input_buffer_bytes)
    return {StatusCode::kLimitExceeded, "parser buffer limit exceeded"};
  if (client.output.size() > client.server->server_config().max_output_buffer_bytes)
    return {StatusCode::kBusy, "output buffer limit exceeded"};
  return Status::Ok();
}

void ClientCoroutine(void *raw) {
  auto client = std::unique_ptr<Client>(static_cast<Client *>(raw));
#ifdef KVSTORE_HAVE_NTYCO
  std::string buffer(16U * 1024U, '\0');
  const auto started = std::chrono::steady_clock::now();
  const auto handshake_started = started;
  auto last_activity = started;
  timeval receive_timeout{0, 100000};
  ::setsockopt(client->fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
               sizeof(receive_timeout));
  ::setsockopt(client->fd, SOL_SOCKET, SO_SNDTIMEO, &receive_timeout,
               sizeof(receive_timeout));
  auto partial_started = std::chrono::steady_clock::time_point::max();
  const auto drain_output = [&client]() {
    std::size_t sent = 0;
    const auto send_deadline = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(client->server->stopping()
                                                             ? client->server->shutdown_remaining_ms()
                                                             : client->server->server_config().idle_timeout_ms);
    while (sent < client->output.size()) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          send_deadline - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) return false;
      const auto result = kvstore_ntyco_send_timed(
          client->fd, client->output.data() + sent, client->output.size() - sent, 0,
          static_cast<unsigned long>(remaining.count()));
      if (result <= 0) return false;
      sent += static_cast<std::size_t>(result);
    }
    client->output.clear();
    return true;
  };
  while (client->fd >= 0) {
    if (client->server->stopping()) {
      const auto cancelled = CommandResponse{false, {}, "CANCELLED server is stopping", false, false, {}, false};
      if (!client->pending_commands.empty()) {
        while (!client->pending_commands.empty()) {
          const auto encoded = client->probe[0] == '*' ? EncodeResp(cancelled) : EncodeNative(cancelled);
          const auto max_output = client->server->server_config().max_output_buffer_bytes;
          if (encoded.size() > max_output - std::min(max_output, client->output.size())) break;
          client->output += encoded;
          client->pending_commands.pop_front();
        }
        client->pending_reject = false;
        client->pending_budget_exhausted = false;
      }
      while (!client->pending_frames.empty()) {
        const auto count = client->pending_frames.front().size();
        const auto encoded = EncodeBatch(std::vector<CommandResponse>(count, cancelled));
        const auto max_output = client->server->server_config().max_output_buffer_bytes;
        if (encoded.size() > max_output - std::min(max_output, client->output.size())) break;
        client->output += encoded;
        client->pending_frames.pop_front();
      }
      client->pending_frame_reject.clear();
      if (!client->output.empty()) static_cast<void>(drain_output());
      break;
    }
    if (!client->pending_commands.empty() || !client->pending_frames.empty()) {
      const auto pending_status = FeedAndServe(*client, {});
      if (!drain_output() || !pending_status.ok()) break;
      continue;
    }
    const auto received = kvstore_ntyco_recv_timed(client->fd, buffer.data(), buffer.size(), 0, 100);
    const auto now = std::chrono::steady_clock::now();
    const auto parser_buffered = client->resp.buffered_bytes() + client->native.buffered_bytes() +
                                 client->batch.buffered_bytes();
    if (parser_buffered != 0 && partial_started == std::chrono::steady_clock::time_point::max())
      partial_started = now;
    if (parser_buffered == 0) partial_started = std::chrono::steady_clock::time_point::max();
    const auto timeout_ms = client->probe.empty()
                                ? client->server->server_config().handshake_timeout_ms
                                : (parser_buffered != 0 ? client->server->protocol_config().parse_timeout_ms
                                                         : client->server->server_config().idle_timeout_ms);
    const auto timeout_start = client->probe.empty() ? handshake_started
                                                     : (parser_buffered != 0 ? partial_started : last_activity);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (std::chrono::duration_cast<std::chrono::milliseconds>(now - timeout_start).count() >=
          static_cast<std::int64_t>(timeout_ms))
        break;
      continue;
    }
    if (received <= 0) {
      if (client->server->stopping() && !client->output.empty())
        static_cast<void>(drain_output());
      break;
    }
    last_activity = now;
    const auto feed_status =
        FeedAndServe(*client, std::string_view(buffer.data(), static_cast<std::size_t>(received)));
    if (!drain_output()) break;
    if (!feed_status.ok()) break;
  }
#endif
  Close(*client);
  if (client->registered) client->server->ReleaseConnection(client.get());
}

struct AcceptContext {
  NtycoServer *server;
  int fd;
};

void AcceptCoroutine(void *raw) {
  auto context = std::unique_ptr<AcceptContext>(static_cast<AcceptContext *>(raw));
#ifdef KVSTORE_HAVE_NTYCO
  std::size_t connection_id = 1;
  while (!context->server->stopping()) {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    const int fd = kvstore_ntyco_accept_timed(context->fd, reinterpret_cast<sockaddr *>(&address),
                                              &length, 100);
    if (fd < 0) {
      if (context->server->stopping()) break;
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      break;
    }
    if (!context->server->TryAcquireConnection()) {
      kvstore_ntyco_close(fd);
      continue;
    }
    auto *client = new (std::nothrow)
        Client(context->server, fd, context->server->protocol_config(), connection_id++);
    if (client == nullptr) {
      context->server->ReleaseUnregisteredConnection();
      kvstore_ntyco_close(fd);
      break;
    }
    client->registered = true;
    context->server->RegisterConnection(client, fd);
    if (kvstore_ntyco_spawn(&ClientCoroutine, client) != 0) {
      context->server->ReleaseConnection(client);
      Close(*client);
      delete client;
      break;
    }
  }
#else
  static_cast<void>(context);
#endif
}

}  // namespace

NtycoServer::NtycoServer(const ServerConfig& config, const ProtocolConfig& protocol,
                         Dispatcher& dispatcher, std::size_t stack_bytes)
    : server_config_(config),
      protocol_config_(protocol),
      dispatcher_(dispatcher),
      stack_bytes_(stack_bytes) {}

NtycoServer::~NtycoServer() { Stop(); }

bool NtycoServer::TryAcquireConnection() noexcept {
  auto current = active_connections_.load(std::memory_order_relaxed);
  while (current < server_config_.max_connections &&
         !active_connections_.compare_exchange_weak(current, current + 1,
                                                     std::memory_order_acq_rel)) {
  }
  return current < server_config_.max_connections;
}

void NtycoServer::RegisterConnection(const void* identity, int fd) {
  std::scoped_lock lock(connections_mutex_);
  client_fds_.emplace(identity, fd);
}

void NtycoServer::ReleaseConnection(const void* identity) noexcept {
  bool owned = false;
  {
    std::scoped_lock lock(connections_mutex_);
    owned = client_fds_.erase(identity) != 0;
  }
  if (owned) active_connections_.fetch_sub(1, std::memory_order_acq_rel);
}

void NtycoServer::ReleaseUnregisteredConnection() noexcept {
  active_connections_.fetch_sub(1, std::memory_order_acq_rel);
}

void NtycoServer::CloseConnections() noexcept {
#ifdef KVSTORE_HAVE_NTYCO
  std::scoped_lock lock(connections_mutex_);
  for (const auto& [_, fd] : client_fds_) {
    ::shutdown(fd, SHUT_RD);
  }
#endif
}

std::uint64_t NtycoServer::shutdown_remaining_ms() const noexcept {
  const auto deadline = shutdown_deadline_ms_.load(std::memory_order_acquire);
  if (deadline == 0) return server_config_.idle_timeout_ms;
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  return deadline > now ? deadline - now : 0;
}


Status NtycoServer::Run() {
#ifndef KVSTORE_HAVE_NTYCO
  state_.store(ServerState::kFailed);
  return {StatusCode::kUnsupported, "NtyCo runtime is not built"};
#else
  if (kvstore_ntyco_init(static_cast<unsigned long>(stack_bytes_)) != 0) {
    state_.store(ServerState::kFailed);
    return {StatusCode::kInternal, "NtyCo scheduler initialization failed"};
  }
  kvstore_ntyco_set_timeout(1000);
  const int socket = kvstore_ntyco_socket(AF_INET, SOCK_STREAM, 0);
  if (socket < 0) {
    state_.store(ServerState::kFailed);
    kvstore_ntyco_run();
    return {StatusCode::kInternal, "NtyCo socket creation failed"};
  }
  int reuse = 1;
  ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(server_config_.port);
  if (::inet_pton(AF_INET, server_config_.listen_address.c_str(), &address.sin_addr) != 1 ||
      ::bind(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
      ::listen(socket, static_cast<int>(server_config_.max_connections)) != 0) {
    kvstore_ntyco_close(socket);
    state_.store(ServerState::kFailed);
    kvstore_ntyco_run();
    return {StatusCode::kInternal, "NtyCo listen setup failed"};
  }
  listen_fd_.store(socket);
  if (stopping_.load()) {
    ::shutdown(socket, SHUT_RDWR);
    kvstore_ntyco_close(socket);
    listen_fd_.store(-1);
    state_.store(ServerState::kStopped);
    kvstore_ntyco_run();
    return Status::Ok();
  }
  auto *context = new AcceptContext{this, socket};
  if (kvstore_ntyco_spawn(&AcceptCoroutine, context) != 0) {
    delete context;
    kvstore_ntyco_close(socket);
    listen_fd_.store(-1);
    state_.store(ServerState::kFailed);
    kvstore_ntyco_run();
    return {StatusCode::kInternal, "NtyCo accept coroutine creation failed"};
  }
  state_.store(ServerState::kReady);
  kvstore_ntyco_run();
  std::scoped_lock listener_lock(listener_mutex_);
  const int owned_listener = listen_fd_.exchange(-1);
  if (owned_listener >= 0) kvstore_ntyco_close(owned_listener);
  state_.store(ServerState::kStopped);
  return Status::Ok();
#endif
}

void NtycoServer::Stop() noexcept {
  if (stopping_.exchange(true, std::memory_order_acq_rel)) return;
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  shutdown_deadline_ms_.store(now + server_config_.graceful_shutdown_ms,
                              std::memory_order_release);
#ifdef KVSTORE_HAVE_NTYCO
  std::scoped_lock listener_lock(listener_mutex_);
  const int fd = listen_fd_.load();
  if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
  CloseConnections();
  kvstore_ntyco_cancel_all();
  kvstore_ntyco_wakeup();
#endif
}

}  // namespace kvstore
