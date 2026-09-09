#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "kvstore/command/dispatcher.hpp"
#include "kvstore/config/config.hpp"

namespace kvstore {

class EpollServer {
 public:
  EpollServer(const ServerConfig& config, const ProtocolConfig& protocol, Dispatcher& dispatcher);
  ~EpollServer();
  [[nodiscard]] Status Run();
  void Stop() noexcept;

 private:
  const ServerConfig& server_config_;
  const ProtocolConfig& protocol_config_;
  Dispatcher& dispatcher_;
  std::atomic<bool> stopping_{false};
  int listen_fd_{-1};
  int epoll_fd_{-1};
};

}  // namespace kvstore
