#pragma once

#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

#include "kvstore/command/command.hpp"
#include "kvstore/common/crc32.hpp"
#include "kvstore/engine/engine.hpp"

namespace kvstore {

struct WriteEvent {
  WriteEvent() = default;
  WriteEvent(std::uint64_t event_offset, std::uint64_t id, WriteSource event_source,
             CommandType command_type, std::string event_key, std::string event_value,
             std::string node = {}, std::uint64_t time_ms = 0, std::uint32_t event_checksum = 0)
      : offset(event_offset),
        event_id(id),
        source(event_source),
        type(command_type),
        key(std::move(event_key)),
        value(std::move(event_value)),
        origin_node(std::move(node)),
        timestamp_ms(time_ms),
        checksum(event_checksum) {
    if (checksum == 0) checksum = ComputeChecksum();
  }
  [[nodiscard]] std::uint32_t ComputeChecksum() const {
    std::string material;
    material.reserve(key.size() + value.size() + origin_node.size() + 64);
    material += std::to_string(offset) + ':' + std::to_string(event_id) + ':';
    material.push_back(static_cast<char>(source));
    material.push_back(static_cast<char>(type));
    material += ':' + std::to_string(timestamp_ms) + ':' + std::to_string(key.size()) + ':';
    material.append(key);
    material += ':' + std::to_string(value.size()) + ':';
    material.append(value);
    material += ':' + std::to_string(origin_node.size()) + ':';
    material.append(origin_node);
    return Crc32(std::as_bytes(std::span(material.data(), material.size())));
  }
  std::uint64_t offset{};
  std::uint64_t event_id{};
  WriteSource source{WriteSource::kClient};
  CommandType type{};
  std::string key;
  std::string value;
  std::string origin_node;
  std::uint64_t timestamp_ms{};
  std::uint32_t checksum{};
};

struct ReplicationSnapshotView {
  std::vector<Entry> entries;
  std::uint64_t offset{};
  std::uint64_t event_id{};
};

using EventSink = std::function<Status(const WriteEvent&)>;
using BatchEventSink = std::function<Status(const std::vector<WriteEvent>&)>;
using ContextEventSink = std::function<Status(const WriteEvent&, const RequestContext&)>;
using ContextBatchEventSink =
    std::function<Status(const std::vector<WriteEvent>&, const RequestContext&)>;
using ManagementAction = std::function<Status()>;
using InfoProvider = std::function<std::string()>;

class Dispatcher {
 public:
  Dispatcher(std::unique_ptr<IEngine> engine, EventSink sink = {}, ManagementAction save = {},
             ManagementAction load = {}, std::uint64_t initial_offset = 1,
             std::uint64_t initial_event_id = 1, std::string origin_node = {});
  void SetManagementActions(ManagementAction save, ManagementAction load);
  void SetBatchEventSink(BatchEventSink sink) { batch_sink_ = std::move(sink); }
  void SetContextEventSinks(ContextEventSink sink, ContextBatchEventSink batch_sink) {
    context_sink_ = std::move(sink);
    context_batch_sink_ = std::move(batch_sink);
  }
  void SetInfoProvider(InfoProvider provider) { info_provider_ = std::move(provider); }
  void SetReadOnly(bool read_only) noexcept { read_only_ = read_only; }
  [[nodiscard]] CommandResponse Execute(const Command& command, const RequestContext& context = {});
  [[nodiscard]] IEngine& engine() noexcept { return *engine_; }
  [[nodiscard]] std::uint64_t next_offset() const noexcept { return next_offset_; }
  [[nodiscard]] std::uint64_t next_event_id() const noexcept { return next_event_id_; }
  [[nodiscard]] Result<ReplicationSnapshotView> SnapshotView(
      std::size_t max_serialized_bytes = std::numeric_limits<std::size_t>::max());
  void SetReplicationPoint(std::uint64_t offset, std::uint64_t event_id);

 private:
  [[nodiscard]] CommandResponse Error(Status status) const;
  [[nodiscard]] CommandResponse Write(CommandType type, const Command& command,
                                      const RequestContext& context);
  [[nodiscard]] CommandResponse Value(std::string value) const;
  [[nodiscard]] Status Publish(CommandType type, const Command& command, std::string_view key,
                               std::string_view value, const RequestContext& context);

  std::unique_ptr<IEngine> engine_;
  std::mutex mutex_;
  EventSink sink_;
  BatchEventSink batch_sink_;
  ContextEventSink context_sink_;
  ContextBatchEventSink context_batch_sink_;
  ManagementAction save_;
  ManagementAction load_;
  InfoProvider info_provider_;
  std::uint64_t next_offset_{1};
  std::uint64_t next_event_id_{1};
  std::string origin_node_;
  bool read_only_{false};
};

}  // namespace kvstore
