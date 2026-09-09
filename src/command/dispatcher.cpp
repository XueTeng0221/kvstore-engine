#include "kvstore/command/dispatcher.hpp"

#include <charconv>
#include <limits>

namespace kvstore {
namespace {

CommandResponse Invalid(std::string message) {
  return {false, {}, std::move(message), false, false, {}};
}
CommandResponse Success() { return {true, "OK", {}, false, false, {}, true}; }
CommandResponse Integer(std::int64_t value) {
  return {true, std::to_string(value), {}, true, false, {}};
}

}  // namespace

Dispatcher::Dispatcher(std::unique_ptr<IEngine> engine, EventSink sink, ManagementAction save,
                       ManagementAction load, std::uint64_t initial_offset,
                       std::uint64_t initial_event_id)
    : engine_(std::move(engine)),
      sink_(std::move(sink)),
      save_(std::move(save)),
      load_(std::move(load)),
      next_offset_(initial_offset),
      next_event_id_(initial_event_id) {}

void Dispatcher::SetManagementActions(ManagementAction save, ManagementAction load) {
  save_ = std::move(save);
  load_ = std::move(load);
}

CommandResponse Dispatcher::Error(Status status) const {
  return Invalid(std::string(status.name()) + " " + std::string(status.message()));
}

CommandResponse Dispatcher::Value(std::string value) const {
  return {true, std::move(value), {}, false, false, {}, false};
}

Status Dispatcher::Publish(CommandType type, const Command& command, std::string_view key,
                           std::string_view value) {
  if (!sink_ || command.source != WriteSource::kClient) return Status::Ok();
  WriteEvent event{next_offset_, next_event_id_,   command.source,
                   type,         std::string(key), std::string(value)};
  const auto status = sink_(event);
  if (status.ok()) {
    ++next_offset_;
    ++next_event_id_;
  }
  return status;
}

CommandResponse Dispatcher::Write(CommandType type, const Command& command) {
  if (command.args.empty()) return Invalid("INVALID_ARGUMENT missing key");
  const auto& key = command.args[0];
  const std::string value = command.args.size() > 1 ? command.args[1] : std::string{};
  Status status;
  std::string previous;
  const auto existing = engine_->Get(key);
  if (existing.ok()) previous = existing.value();
  if (type == CommandType::kSet) {
    if (command.redis_compat) {
      auto result = engine_->Upsert(key, value);
      if (!result.ok()) return Error(result.status());
    } else {
      status = engine_->Create(key, value);
    }
  }
  if (type == CommandType::kMod) status = engine_->Modify(key, value);
  if (type == CommandType::kDel) {
    status = engine_->Delete(key);
    if (command.redis_compat && status.code() == StatusCode::kNotFound) return Integer(0);
  }
  if (!status.ok()) return Error(status);
  status = Publish(type, command, key, value);
  if (!status.ok()) {
    if (type == CommandType::kDel) {
      if (!previous.empty() || existing.ok()) static_cast<void>(engine_->Create(key, previous));
    } else if (existing.ok()) {
      static_cast<void>(engine_->Modify(key, previous));
    } else {
      static_cast<void>(engine_->Delete(key));
    }
    return Error(status);
  }
  return type == CommandType::kDel ? Integer(1) : Success();
}

CommandResponse Dispatcher::Execute(const Command& command) {
  std::scoped_lock lock(mutex_);
  const auto arity = command.args.size();
  switch (command.type) {
    case CommandType::kSet:
    case CommandType::kMod:
      if (arity != 2) return Invalid("INVALID_ARGUMENT expected key and value");
      return Write(command.type, command);
    case CommandType::kDel:
      if (arity != 1) return Invalid("INVALID_ARGUMENT expected key");
      return Write(command.type, command);
    case CommandType::kGet: {
      if (arity != 1) return Invalid("INVALID_ARGUMENT expected key");
      auto result = engine_->Get(command.args[0]);
      if (result.ok()) return Value(std::move(result).value());
      if (result.status().code() == StatusCode::kNotFound)
        return {true, {}, {}, false, true, {}, false};
      return Error(result.status());
    }
    case CommandType::kExist: {
      if (arity != 1) return Invalid("INVALID_ARGUMENT expected key");
      auto result = engine_->Exists(command.args[0]);
      return result.ok() ? Integer(result.value() ? 1 : 0) : Error(result.status());
    }
    case CommandType::kIncr:
    case CommandType::kDecr: {
      if (arity != 1 && arity != 2) return Invalid("INVALID_ARGUMENT expected key [delta]");
      const auto existing = engine_->Get(command.args[0]);
      std::int64_t delta = command.type == CommandType::kDecr ? -1 : 1;
      if (arity == 2) {
        auto parsed = std::from_chars(command.args[1].data(),
                                      command.args[1].data() + command.args[1].size(), delta);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != command.args[1].data() + command.args[1].size()) {
          return Invalid("INVALID_ARGUMENT invalid delta");
        }
        if (command.type == CommandType::kDecr) {
          if (delta == std::numeric_limits<std::int64_t>::min())
            return Invalid("INVALID_ARGUMENT overflow");
          delta = -delta;
        }
      }
      auto result = engine_->Increment(command.args[0], delta);
      if (!result.ok()) return Error(result.status());
      std::string event_value = arity == 2
                                    ? command.args[1]
                                    : std::to_string(command.type == CommandType::kDecr ? -1 : 1);
      if (command.type == CommandType::kDecr && arity == 2) {
        std::int64_t requested = 0;
        const auto [end, error] =
            std::from_chars(event_value.data(), event_value.data() + event_value.size(), requested);
        if (error != std::errc{} || end != event_value.data() + event_value.size() ||
            requested == std::numeric_limits<std::int64_t>::min())
          return Invalid("INVALID_ARGUMENT overflow");
        event_value = std::to_string(-requested);
      }
      const auto status = Publish(command.type, command, command.args[0], event_value);
      if (!status.ok()) {
        if (existing.ok())
          static_cast<void>(engine_->Modify(command.args[0], existing.value()));
        else
          static_cast<void>(engine_->Delete(command.args[0]));
        return Error(status);
      }
      return Integer(result.value());
    }
    case CommandType::kMget: {
      if (arity == 0) return Invalid("INVALID_ARGUMENT expected keys");
      CommandResponse output{true, {}, {}, false, false, {}};
      for (const auto& key : command.args) {
        auto result = engine_->Get(key);
        if (result.ok())
          output.array.push_back(Value(std::move(result).value()));
        else if (result.status().code() == StatusCode::kNotFound)
          output.array.push_back({true, {}, {}, false, true, {}});
        else
          return Error(result.status());
      }
      return output;
    }
    case CommandType::kPing:
      if (arity > 1) return Invalid("INVALID_ARGUMENT too many arguments");
      return arity == 0 ? CommandResponse{true, "PONG", {}, false, false, {}, true}
                        : Value(command.args[0]);
    case CommandType::kEcho:
      return arity == 1 ? Value(command.args[0]) : Invalid("INVALID_ARGUMENT expected message");
    case CommandType::kClient:
      return Success();
    case CommandType::kInfo:
      return Value("# Server\r\nkvstore_version:0.1.0\r\n");
    case CommandType::kSave: {
      if (arity != 0 || !save_) return Invalid("UNSUPPORTED SAVE is not attached");
      const auto status = save_();
      return status.ok() ? Success() : Error(status);
    }
    case CommandType::kLoad: {
      if (arity != 0 || !load_) return Invalid("UNSUPPORTED LOAD is not attached");
      const auto status = load_();
      return status.ok() ? Success() : Error(status);
    }
  }
  return Invalid("INTERNAL unknown command");
}

}  // namespace kvstore
