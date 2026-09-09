#pragma once

#include <functional>
#include <memory>
#include <mutex>

#include "kvstore/command/command.hpp"
#include "kvstore/engine/engine.hpp"

namespace kvstore {

struct WriteEvent {
  std::uint64_t offset{};
  std::uint64_t event_id{};
  WriteSource source{WriteSource::kClient};
  CommandType type{};
  std::string key;
  std::string value;
};

using EventSink = std::function<Status(const WriteEvent&)>;
using ManagementAction = std::function<Status()>;

class Dispatcher {
 public:
  Dispatcher(std::unique_ptr<IEngine> engine, EventSink sink = {}, ManagementAction save = {},
             ManagementAction load = {}, std::uint64_t initial_offset = 1,
             std::uint64_t initial_event_id = 1);
  void SetManagementActions(ManagementAction save, ManagementAction load);
  [[nodiscard]] CommandResponse Execute(const Command& command);
  [[nodiscard]] IEngine& engine() noexcept { return *engine_; }
  [[nodiscard]] std::uint64_t next_offset() const noexcept { return next_offset_; }

 private:
  [[nodiscard]] CommandResponse Error(Status status) const;
  [[nodiscard]] CommandResponse Write(CommandType type, const Command& command);
  [[nodiscard]] CommandResponse Value(std::string value) const;
  [[nodiscard]] Status Publish(CommandType type, const Command& command, std::string_view key,
                               std::string_view value);

  std::unique_ptr<IEngine> engine_;
  std::mutex mutex_;
  EventSink sink_;
  ManagementAction save_;
  ManagementAction load_;
  std::uint64_t next_offset_{1};
  std::uint64_t next_event_id_{1};
};

}  // namespace kvstore
