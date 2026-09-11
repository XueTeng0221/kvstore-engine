#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string_view>

#include "kvstore/command/dispatcher.hpp"
#include "kvstore/config/config.hpp"
#include "kvstore/engine/engine_factory.hpp"
#include "kvstore/net/epoll_server.hpp"
#include "kvstore/persistence/aof.hpp"
#include "kvstore/persistence/snapshot.hpp"
#include "kvstore/persistence/write_event_queue.hpp"
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
  if (config.value().persistence.io_uring_write && config.value().persistence.allow_sync_fallback)
    std::cerr << "notice: io_uring snapshot requested; synchronous fallback is permitted\n";
  std::unique_ptr<kvstore::AofWriter> aof;
  if (config.value().persistence.aof_enabled) {
    aof = std::make_unique<kvstore::AofWriter>(
        persistence_directory, config.value().persistence.flush_records,
        config.value().persistence.flush_bytes, config.value().persistence.flush_interval_ms,
        config.value().persistence.sync_policy, config.value().persistence.max_aof_bytes);
    if (!config.value().persistence.recover_on_start && std::filesystem::exists(aof->path()) &&
        std::filesystem::file_size(aof->path()) != 0) {
      std::cerr << "INVALID_ARGUMENT: recover_on_start=false requires an empty AOF\n";
      return 1;
    }
  }
  std::uint64_t recovered_offset = 0;
  std::uint64_t recovered_event_id = 0;
  const auto recovery_started = std::chrono::steady_clock::now();
  if (config.value().persistence.recover_on_start) {
    auto snapshot_offset =
        config.value().persistence.snapshot_enabled
            ? kvstore::Snapshot::LoadWithMetadata(persistence_directory, *engine.value())
            : kvstore::Result<kvstore::RecoveryPoint>(kvstore::RecoveryPoint{});
    if (!snapshot_offset.ok()) {
      std::cerr << snapshot_offset.status().name() << ": " << snapshot_offset.status().message()
                << '\n';
      return 1;
    }
    const auto replay_status = aof ? aof->Replay(*engine.value(), snapshot_offset.value().offset,
                                                 snapshot_offset.value().event_id)
                                   : kvstore::Result<std::uint64_t>(std::uint64_t{0});
    if (!replay_status.ok()) {
      std::cerr << replay_status.status().name() << ": " << replay_status.status().message()
                << '\n';
      return 1;
    }
    if (replay_status.value() != 0 && aof &&
        aof->last_event_id() < snapshot_offset.value().event_id) {
      std::cerr << "CORRUPTION: AOF event id is behind snapshot\n";
      return 1;
    }
    recovered_offset = std::max(snapshot_offset.value().offset, replay_status.value());
    recovered_event_id =
        std::max(snapshot_offset.value().event_id, aof ? aof->last_event_id() : 0U);
  }
  std::unique_ptr<kvstore::WriteEventQueue> event_queue;
  if (aof)
    event_queue = std::make_unique<kvstore::WriteEventQueue>(
        std::max(config.value().persistence.flush_records,
                 config.value().protocol.max_batch_records),
        std::max(config.value().persistence.flush_bytes, config.value().protocol.max_frame_bytes),
        [&aof](const std::vector<kvstore::WriteEvent>& events) {
          return aof->AppendBatch(events);
        });
  kvstore::EventSink event_sink;
  if (event_queue)
    event_sink = [&event_queue](const kvstore::WriteEvent& event) {
      return event_queue->Submit(event);
    };
  kvstore::Dispatcher dispatcher(std::move(engine).value(), std::move(event_sink), {}, {},
                                 recovered_offset + 1, recovered_event_id + 1,
                                 config.value().replication.node_id);
  if (event_queue)
    dispatcher.SetContextEventSinks(
        [&event_queue](const kvstore::WriteEvent& event, const kvstore::RequestContext& context) {
          return event_queue->Submit(event, context.deadline, context.stop);
        },
        [&event_queue](const std::vector<kvstore::WriteEvent>& events,
                       const kvstore::RequestContext& context) {
          return event_queue->SubmitBatch(events, context.deadline, context.stop);
        });
  const auto recovery_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - recovery_started)
                                        .count();
  dispatcher.SetInfoProvider([recovered_offset, recovered_event_id, recovery_duration_ms,
                              aof_enabled = static_cast<bool>(aof)] {
    return "# Persistence\r\naof_enabled:" + std::to_string(aof_enabled ? 1 : 0) +
           "\r\nrecovery_phase:complete\r\nrecovery_offset:" + std::to_string(recovered_offset) +
           "\r\nrecovery_event_id:" + std::to_string(recovered_event_id) +
           "\r\nrecovery_duration_ms:" + std::to_string(recovery_duration_ms) + "\r\n";
  });
  if (event_queue)
    dispatcher.SetBatchEventSink([&event_queue](const std::vector<kvstore::WriteEvent>& events) {
      return event_queue->SubmitBatch(events);
    });
  if (config.value().persistence.snapshot_enabled)
    dispatcher.SetManagementActions(
        [&dispatcher, &persistence_directory, &config]() {
          return kvstore::Snapshot::Save(
              persistence_directory, dispatcher.engine(), dispatcher.next_offset() - 1,
              dispatcher.next_event_id() - 1, config.value().persistence.io_uring_write,
              config.value().persistence.allow_sync_fallback);
        },
        aof ? kvstore::ManagementAction{}
            : kvstore::ManagementAction{[&dispatcher, &persistence_directory]() {
                const auto loaded =
                    kvstore::Snapshot::Load(persistence_directory, dispatcher.engine());
                return loaded.ok() ? kvstore::Status::Ok() : loaded.status();
              }});
  kvstore::EpollServer server(config.value().server, config.value().protocol, dispatcher,
                              config.value().network.epoll_max_events);
  g_server = &server;
  std::signal(SIGINT, StopServer);
  std::signal(SIGTERM, StopServer);
  const auto status = server.Run();
  g_server = nullptr;
  return status.ok() ? 0 : 1;
}
