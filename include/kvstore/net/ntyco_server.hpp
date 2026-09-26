#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "kvstore/net/epoll_server.hpp"

namespace kvstore {

class NtycoServer final : public INetworkBackend {
 public:
  NtycoServer(const ServerConfig& config, const ProtocolConfig& protocol, Dispatcher& dispatcher,
              std::size_t stack_bytes = 128U * 1024U);
  ~NtycoServer() override;

  [[nodiscard]] Status Run() override;
  void Stop() noexcept override;

  [[nodiscard]] bool ready() const noexcept { return state_.load() == ServerState::kReady; }
  [[nodiscard]] ServerState state() const noexcept { return state_.load(); }
  [[nodiscard]] const ServerConfig& server_config() const noexcept { return server_config_; }
  [[nodiscard]] const ProtocolConfig& protocol_config() const noexcept { return protocol_config_; }
  [[nodiscard]] Dispatcher& dispatcher() noexcept { return dispatcher_; }
  [[nodiscard]] bool stopping() const noexcept { return stopping_.load(); }
  [[nodiscard]] bool TryAcquireConnection() noexcept;
  void ReleaseConnection(const void* identity) noexcept;
  void ReleaseUnregisteredConnection() noexcept;
  void RegisterConnection(const void* identity, int fd);
  void CloseConnections() noexcept;
  [[nodiscard]] std::mutex& execution_mutex() noexcept { return execution_mutex_; }
  [[nodiscard]] std::uint64_t shutdown_remaining_ms() const noexcept;

 private:
  const ServerConfig& server_config_;
  const ProtocolConfig& protocol_config_;
  Dispatcher& dispatcher_;
  std::size_t stack_bytes_;
  std::atomic<bool> stopping_{false};
  std::atomic<ServerState> state_{ServerState::kStarting};
  std::atomic<int> listen_fd_{-1};
  mutable std::mutex listener_mutex_;
  std::atomic<std::size_t> active_connections_{0};
  mutable std::mutex connections_mutex_;
  std::unordered_map<const void*, int> client_fds_;
  std::mutex execution_mutex_;
  std::atomic<std::uint64_t> shutdown_deadline_ms_{0};
};

}  // namespace kvstore
