#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "kvstore/integration/uds_bridge.hpp"
#include "kvstore/kvcache/chunk_registry.hpp"
#include "kvstore/kvcache/match_index.hpp"

int main(int argc, char** argv) {
  if (argc < 4 || argc > 6) return 2;
  kvstore::kvcache::ChunkRegistry registry;
  kvstore::kvcache::MatchIndex index;
  std::string path = argv[1];
  const std::string mode = argc >= 5 ? argv[4] : "";
  const std::filesystem::path disk_directory = mode == "disk" && argc == 6 ? argv[5] : "";
  if (mode == "nul") path.append("\0bad", 4);
  kvstore::integration::UdsBridge bridge(registry, index, path, argv[2], argv[3],
                                        disk_directory);
  if (mode == "nul") {
    if (bridge.Start().code() != kvstore::StatusCode::kInvalidArgument) return 1;
    std::cout << "NUL REJECTED" << std::endl;
    return 0;
  }
  if (mode == "unsafe-parent") {
    if (bridge.Start().code() != kvstore::StatusCode::kInvalidArgument) return 1;
    std::cout << "UNSAFE PARENT REJECTED" << std::endl;
    return 0;
  }
  if (mode == "concurrent") {
    for (int round = 0; round < 32; ++round) {
      // Alternate initially running/stopped states; release both callers from
      // an acknowledged gate, without sleeps or assuming mutex acquisition order.
      if (round % 2 == 0 && !bridge.Start().ok()) return 1;
      std::mutex mutex;
      std::condition_variable changed;
      int ready = 0;
      int done = 0;
      bool release = false;
      kvstore::Status start_status;
      auto run = [&](bool start) {
        {
          std::unique_lock lock(mutex);
          ++ready;
          changed.notify_all();
          if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return release; }))
            std::abort();
        }
        if (start)
          start_status = bridge.Start();
        else
          bridge.Stop();
        std::lock_guard lock(mutex);
        ++done;
        changed.notify_all();
      };
      std::thread starter(run, true);
      std::thread stopper(run, false);
      {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return ready == 2; }))
          std::abort();
        release = true;
        changed.notify_all();
        // Abort on a lifecycle deadlock instead of blocking in thread destructors.
        if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return done == 2; }))
          std::abort();
      }
      starter.join();
      stopper.join();
      if (!start_status.ok() && start_status.code() != kvstore::StatusCode::kAlreadyExists)
        return 1;
      bridge.Stop();
      if (std::filesystem::exists(path)) return 1;
      if (!bridge.Start().ok()) return 1;
      bridge.Stop();
      bridge.Stop();
      if (std::filesystem::exists(path)) return 1;
    }
    std::cout << "CONCURRENT 32" << std::endl;
    return 0;
  }
  const auto status = bridge.Start();
  if (!status.ok()) {
    std::cerr << status.message() << std::endl;
    return 1;
  }
  std::cout << "READY" << std::endl;
  std::string command;
  std::getline(std::cin, command);
  bridge.Stop();
  return 0;
}
