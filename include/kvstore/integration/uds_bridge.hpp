#pragma once

#include <sys/types.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kvstore/integration/protocol.hpp"
#include "kvstore/kvcache/tiered_store.hpp"

namespace kvstore::integration {

// Versioned, single-tenant UDS endpoint for framework adapters. Authentication
// is performed by the trusted server which chooses tenant/model before start.
class UdsBridge final {
 public:
  UdsBridge(kvcache::ChunkRegistry& registry, kvcache::MatchIndex& index, std::string path,
            std::string tenant, std::string model,
            std::filesystem::path disk_directory = {});
  ~UdsBridge();
  UdsBridge(const UdsBridge&) = delete;
  UdsBridge& operator=(const UdsBridge&) = delete;
  [[nodiscard]] Status Start();
  void Stop() noexcept;

 private:
  void Run() noexcept;
  void HandleClient(int client) noexcept;
  struct Worker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  kvcache::ChunkRegistry& registry_;
  kvcache::MatchIndex& index_;
  std::string path_, tenant_, model_;
  std::filesystem::path disk_directory_;
  std::unique_ptr<kvcache::TieredStore> tiered_store_;
  std::atomic<bool> stopping_{false};
  std::atomic<int> listen_fd_{-1};
  std::mutex listener_mutex_;
  std::thread thread_;
  std::mutex clients_mutex_;
  std::vector<int> clients_;
  std::vector<Worker> client_threads_;
  std::mutex lifecycle_mutex_;
  static constexpr std::size_t kMaxClients = 32;
  bool owns_path_{false};
  dev_t owned_dev_{}, owned_ino_{};
  int owned_parent_fd_{-1};
  std::string owned_name_;
};
}  // namespace kvstore::integration
