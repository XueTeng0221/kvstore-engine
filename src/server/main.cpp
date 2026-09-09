#include <algorithm>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string_view>

#include "kvstore/command/dispatcher.hpp"
#include "kvstore/config/config.hpp"
#include "kvstore/engine/engine_factory.hpp"
#include "kvstore/net/epoll_server.hpp"
#include "kvstore/persistence/aof.hpp"
#include "kvstore/persistence/snapshot.hpp"
#include "kvstore/version.hpp"

namespace {

void PrintUsage(std::ostream& output) {
  output << "Usage: kvstore_server [--config PATH] [--help] [--version] [--check-config]\n";
}

kvstore::EpollServer* g_server = nullptr;
void StopServer(int) {
  if (g_server != nullptr) g_server->Stop();
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path = "config/config.json";
  bool check_config = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help") {
      PrintUsage(std::cout);
      return 0;
    }
    if (argument == "--version") {
      std::cout << KVSTORE_VERSION << '\n';
      return 0;
    }
    if (argument == "--check-config") {
      check_config = true;
      continue;
    }
    if (argument == "--config" && index + 1 < argc) {
      config_path = argv[++index];
      continue;
    }
    std::cerr << "unknown or incomplete argument: " << argument << '\n';
    PrintUsage(std::cerr);
    return 2;
  }
  auto config = kvstore::Config::Load(config_path);
  if (!config.ok()) {
    std::cerr << config.status().name() << ": " << config.status().message() << '\n';
    return 1;
  }
  if (check_config) {
    std::cout << config.value().ToRedactedJson() << '\n';
    return 0;
  }
  auto engine = kvstore::CreateEngine(config.value().engine);
  if (!engine.ok()) {
    std::cerr << engine.status().name() << ": " << engine.status().message() << '\n';
    return 1;
  }
  const auto persistence_directory =
      config.value().persistence.directory.is_absolute()
          ? config.value().persistence.directory
          : std::filesystem::path(config_path).parent_path() / config.value().persistence.directory;
  kvstore::AofWriter aof(persistence_directory, config.value().persistence.flush_records,
                         config.value().persistence.flush_bytes,
                         config.value().persistence.flush_interval_ms,
                         config.value().persistence.sync_policy);
  std::uint64_t recovered_offset = 0;
  if (config.value().persistence.recover_on_start) {
    auto snapshot_offset = kvstore::Snapshot::Load(persistence_directory, *engine.value());
    if (!snapshot_offset.ok()) {
      std::cerr << snapshot_offset.status().name() << ": " << snapshot_offset.status().message()
                << '\n';
      return 1;
    }
    const auto replay_status = aof.Replay(*engine.value(), snapshot_offset.value());
    if (!replay_status.ok()) {
      std::cerr << replay_status.status().name() << ": " << replay_status.status().message()
                << '\n';
      return 1;
    }
    recovered_offset = std::max(snapshot_offset.value(), replay_status.value());
  }
  kvstore::Dispatcher dispatcher(
      std::move(engine).value(),
      [&aof](const kvstore::WriteEvent& event) { return aof.Append(event); }, {}, {},
      recovered_offset + 1, recovered_offset + 1);
  dispatcher.SetManagementActions(
      [&dispatcher, &persistence_directory]() {
        return kvstore::Snapshot::Save(persistence_directory, dispatcher.engine(),
                                       dispatcher.next_offset() - 1);
      },
      [&dispatcher, &persistence_directory]() {
        const auto loaded = kvstore::Snapshot::Load(persistence_directory, dispatcher.engine());
        return loaded.ok() ? kvstore::Status::Ok() : loaded.status();
      });
  kvstore::EpollServer server(config.value().server, config.value().protocol, dispatcher);
  g_server = &server;
  std::signal(SIGINT, StopServer);
  std::signal(SIGTERM, StopServer);
  const auto status = server.Run();
  g_server = nullptr;
  return status.ok() ? 0 : 1;
}
