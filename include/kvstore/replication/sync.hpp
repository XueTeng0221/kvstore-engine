#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "kvstore/command/dispatcher.hpp"
#include "kvstore/engine/engine.hpp"
#include "kvstore/persistence/snapshot.hpp"
#include "kvstore/replication/backlog.hpp"

namespace kvstore {

struct ReplicationAck {
  std::uint64_t offset{};
  std::uint64_t event_id{};
};

class PrimarySyncSession {
 public:
  PrimarySyncSession(IEngine& engine, ReplicationBacklog& backlog,
                     std::filesystem::path snapshot_directory);
  [[nodiscard]] Result<RecoveryPoint> BeginFullSync();
  [[nodiscard]] Result<std::vector<WriteEvent>> CatchUp(std::uint64_t next_offset) const;
  [[nodiscard]] Status Ack(ReplicationAck ack);
  [[nodiscard]] Status Heartbeat(std::chrono::steady_clock::time_point now);
  [[nodiscard]] ReplicationAck acknowledged() const;
  [[nodiscard]] RecoveryPoint snapshot_point() const;

 private:
  IEngine& engine_;
  ReplicationBacklog& backlog_;
  mutable std::mutex mutex_;
  std::filesystem::path snapshot_directory_;
  RecoveryPoint snapshot_point_{};
  ReplicationAck acknowledged_{};
  std::chrono::steady_clock::time_point last_heartbeat_{};
};

class ReplicaSyncApplier {
 public:
  explicit ReplicaSyncApplier(IEngine& engine) : engine_(engine) {}
  [[nodiscard]] Status InstallFullSync(const std::vector<Entry>& entries, RecoveryPoint point);
  [[nodiscard]] Status ApplyIncremental(const std::vector<WriteEvent>& events);
  [[nodiscard]] Status Ack(ReplicationAck ack);
  [[nodiscard]] ReplicationAck applied() const;

 private:
  [[nodiscard]] Status Apply(const WriteEvent& event);
  IEngine& engine_;
  mutable std::mutex mutex_;
  ReplicationAck applied_{};
  std::uint32_t applied_checksum_{0};
};

}  // namespace kvstore
