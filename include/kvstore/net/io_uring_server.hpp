#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>

#include "kvstore/net/epoll_server.hpp"

namespace kvstore {

class IoUringServer final : public INetworkBackend {
 public:
  IoUringServer(const ServerConfig& config, const ProtocolConfig& protocol, Dispatcher& dispatcher,
                std::string replication_role = "primary", std::string replication_node_id = {},
                std::string replication_upstream = {},
                ReplicationBacklog* replication_backlog = nullptr,
                std::uint64_t replication_heartbeat_ms = 1000, std::size_t max_events = 1024,
                IReplicationExecutor* replication_executor = nullptr,
                bool allow_peer_cycles = false, std::size_t queue_depth = 1024, bool sqpoll = false,
                bool allow_fallback = false);
  ~IoUringServer() override;

  [[nodiscard]] Status Run() override;
  void Stop() noexcept override;

  [[nodiscard]] bool ready() const noexcept {
    std::lock_guard lock(fallback_mutex_);
    return fallback_ != nullptr ? fallback_->ready()
                                : state_.load(std::memory_order_acquire) == ServerState::kReady;
  }
  [[nodiscard]] ServerState state() const noexcept {
    std::lock_guard lock(fallback_mutex_);
    return fallback_ == nullptr ? state_.load(std::memory_order_acquire) : fallback_->state();
  }
  [[nodiscard]] static Status Probe(std::size_t queue_depth, bool sqpoll) noexcept;

 private:
  const ServerConfig& server_config_;
  const ProtocolConfig& protocol_config_;
  Dispatcher& dispatcher_;
  std::string replication_role_;
  std::string replication_node_id_;
  std::string replication_upstream_;
  ReplicationBacklog* replication_backlog_;
  std::uint64_t replication_heartbeat_ms_;
  std::size_t max_events_;
  IReplicationExecutor* replication_executor_;
  bool allow_peer_cycles_;
  std::size_t queue_depth_;
  bool sqpoll_;
  bool allow_fallback_;
  std::atomic<bool> stopping_{false};
  std::atomic<ServerState> state_{ServerState::kStarting};
  mutable std::mutex fallback_mutex_;
  std::shared_ptr<EpollServer> fallback_;
};

}  // namespace kvstore
