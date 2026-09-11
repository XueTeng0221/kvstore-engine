#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "kvstore/command/dispatcher.hpp"
#include "kvstore/config/config.hpp"
#include "kvstore/net/network_backend.hpp"

namespace kvstore {

enum class ServerState { kStarting, kReady, kDraining, kStopped, kFailed };

class EpollServer final : public INetworkBackend {
 public:
  EpollServer(const ServerConfig& config, const ProtocolConfig& protocol, Dispatcher& dispatcher,
              std::size_t max_events = 1024);
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
  std::size_t max_events_;
  std::atomic<bool> stopping_{false};
  std::atomic<ServerState> state_{ServerState::kStarting};
  int listen_fd_{-1};
  int epoll_fd_{-1};
};

}  // namespace kvstore
