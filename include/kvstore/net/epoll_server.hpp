#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "kvstore/command/dispatcher.hpp"
#include "kvstore/config/config.hpp"
#include "kvstore/net/network_backend.hpp"
#include "kvstore/replication/backlog.hpp"
#include "kvstore/replication/executor.hpp"
#include "kvstore/replication/live_sync.hpp"

namespace kvstore {

enum class ServerState { kStarting, kReady, kDraining, kStopped, kFailed };

class EpollServer final : public INetworkBackend {
 public:
  EpollServer(const ServerConfig& config, const ProtocolConfig& protocol, Dispatcher& dispatcher,
              std::string replication_role = "primary", std::string replication_node_id = {},
              std::string replication_upstream = {},
               ReplicationBacklog* replication_backlog = nullptr,
               std::uint64_t replication_heartbeat_ms = 1000, std::size_t max_events = 1024,
               IReplicationExecutor* replication_executor = nullptr,
               bool allow_peer_cycles = false);
  EpollServer(const ServerConfig& config, const ProtocolConfig& protocol, Dispatcher& dispatcher,
              std::size_t max_events);
  ~EpollServer() override;
  [[nodiscard]] Status Run() override;
  void Stop() noexcept override;
  [[nodiscard]] bool ready() const noexcept {
    return state_.load(std::memory_order_acquire) == ServerState::kReady;
  }
  [[nodiscard]] bool live() const noexcept {
    const auto state = state_.load(std::memory_order_acquire);
    return state != ServerState::kStopped && state != ServerState::kFailed;
  }
  [[nodiscard]] ServerState state() const noexcept {
    return state_.load(std::memory_order_acquire);
  }

 private:
  const ServerConfig& server_config_;
  const ProtocolConfig& protocol_config_;
  Dispatcher& dispatcher_;
  std::string replication_role_;
  std::string replication_node_id_;
  std::string replication_upstream_;
  std::chrono::milliseconds replication_heartbeat_interval_;
  ReplicationBacklog* replication_backlog_;
  IReplicationExecutor* replication_executor_;
  const bool allow_peer_cycles_;
  LiveSyncDeduplicator live_sync_deduplicator_{4096, std::chrono::minutes(10)};
  std::size_t max_events_;
  std::atomic<bool> stopping_{false};
  std::atomic<ServerState> state_{ServerState::kStarting};
  int listen_fd_{-1};
  int epoll_fd_{-1};
};

}  // namespace kvstore
