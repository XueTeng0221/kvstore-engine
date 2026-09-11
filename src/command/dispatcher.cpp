#include "kvstore/command/dispatcher.hpp"

#include <cctype>
#include <charconv>
#include <chrono>
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

std::string EncodeKeys(const std::vector<Entry>& entries) {
  std::string output;
  for (const auto& entry : entries) {
    const auto size = static_cast<std::uint32_t>(entry.key.size());
    for (std::size_t index = 0; index < sizeof(size); ++index)
      output.push_back(static_cast<char>(size >> ((sizeof(size) - index - 1U) * 8U)));
    output += entry.key;
  }
  return output;
}

}  // namespace

Dispatcher::Dispatcher(std::unique_ptr<IEngine> engine, EventSink sink, ManagementAction save,
                       ManagementAction load, std::uint64_t initial_offset,
                       std::uint64_t initial_event_id, std::string origin_node)
    : engine_(std::move(engine)),
      sink_(std::move(sink)),
      save_(std::move(save)),
      load_(std::move(load)),
      next_offset_(initial_offset),
      next_event_id_(initial_event_id),
      origin_node_(std::move(origin_node)) {}

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
                           std::string_view value, const RequestContext& context) {
  if (command.source != WriteSource::kClient) return Status::Ok();
  WriteEvent event{
      next_offset_,
      next_event_id_,
      command.source,
      type,
      std::string(key),
      std::string(value),
      origin_node_,
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count()),
      0};
  const auto status =
      context_sink_ ? context_sink_(event, context) : (sink_ ? sink_(event) : Status::Ok());
  if (status.ok() || status.code() == StatusCode::kInternal) {
    ++next_offset_;
    ++next_event_id_;
  }
  return status;
}

CommandResponse Dispatcher::Write(CommandType type, const Command& command,
                                  const RequestContext& context) {
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
  status = Publish(type, command, key, value, context);
  if (!status.ok()) {
    if (status.code() == StatusCode::kInternal) return Error(status);
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

CommandResponse Dispatcher::Execute(const Command& command, const RequestContext& context) {
  if (context.stop.stop_requested()) return Invalid("CANCELLED request cancelled");
  if (std::chrono::steady_clock::now() >= context.deadline)
    return Invalid("CANCELLED request deadline exceeded");
  std::scoped_lock lock(mutex_);
  if (context.stop.stop_requested()) return Invalid("CANCELLED request cancelled");
  if (std::chrono::steady_clock::now() >= context.deadline)
    return Invalid("CANCELLED request deadline exceeded");
  const auto arity = command.args.size();
  switch (command.type) {
    case CommandType::kSet:
    case CommandType::kMod:
      if (arity != 2) return Invalid("INVALID_ARGUMENT expected key and value");
      return Write(command.type, command, context);
    case CommandType::kDel:
      if (!command.redis_compat && arity != 1) return Invalid("INVALID_ARGUMENT expected key");
      if (command.redis_compat && arity == 0) return Invalid("INVALID_ARGUMENT expected key");
      if (command.redis_compat) {
        std::int64_t deleted = 0;
        std::vector<Entry> removed;
        removed.reserve(command.args.size());
        for (const auto& key : command.args) {
          const auto previous = engine_->Get(key);
          if (!previous.ok() && previous.status().code() != StatusCode::kNotFound)
            return Error(previous.status());
          const auto status = engine_->Delete(key);
          if (status.ok()) {
            if (previous.ok()) removed.push_back({key, previous.value()});
            ++deleted;
          } else if (status.code() != StatusCode::kNotFound) {
            for (const auto& entry : removed)
              static_cast<void>(engine_->Upsert(entry.key, entry.value));
            return Error(status);
          }
        }
        if (removed.empty()) return Integer(0);
        const auto published =
            Publish(CommandType::kDelMany, command, {}, EncodeKeys(removed), context);
        if (!published.ok()) {
          if (published.code() == StatusCode::kInternal) return Error(published);
          for (const auto& entry : removed)
            static_cast<void>(engine_->Upsert(entry.key, entry.value));
          return Error(published);
        }
        return Integer(deleted);
      }
      return Write(command.type, command, context);
    case CommandType::kGet: {
      if (arity != 1) return Invalid("INVALID_ARGUMENT expected key");
      auto result = engine_->Get(command.args[0]);
      if (result.ok()) {
        if (result.value().size() + 64U > context.response_budget)
          return Invalid("BUSY response exceeds connection limit");
        return Value(std::move(result).value());
      }
      if (command.redis_compat && result.status().code() == StatusCode::kNotFound)
        return {true, {}, {}, false, true, {}, false};
      return Error(result.status());
    }
    case CommandType::kExist: {
      if (!command.redis_compat && arity != 1) return Invalid("INVALID_ARGUMENT expected key");
      if (command.redis_compat && arity == 0) return Invalid("INVALID_ARGUMENT expected key");
      std::int64_t count = 0;
      for (const auto& key : command.args) {
        auto result = engine_->Exists(key);
        if (!result.ok()) return Error(result.status());
        if (result.value()) ++count;
      }
      return Integer(command.redis_compat ? count : (count != 0 ? 1 : 0));
    }
    case CommandType::kIncr:
    case CommandType::kDecr:
    case CommandType::kIncrBy:
    case CommandType::kDecrBy: {
      const bool by = command.type == CommandType::kIncrBy || command.type == CommandType::kDecrBy;
      const bool decrement =
          command.type == CommandType::kDecr || command.type == CommandType::kDecrBy;
      if ((by && arity != 2) || (!by && command.redis_compat && arity != 1) ||
          (!by && !command.redis_compat && arity != 1 && arity != 2))
        return Invalid("INVALID_ARGUMENT expected key");
      const auto existing = engine_->Get(command.args[0]);
      std::int64_t delta = decrement ? -1 : 1;
      if (arity == 2) {
        auto parsed = std::from_chars(command.args[1].data(),
                                      command.args[1].data() + command.args[1].size(), delta);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != command.args[1].data() + command.args[1].size()) {
          return Invalid("INVALID_ARGUMENT invalid delta");
        }
        if (decrement) {
          if (delta == std::numeric_limits<std::int64_t>::min())
            return Invalid("INVALID_ARGUMENT overflow");
          delta = -delta;
        }
      }
      auto result = engine_->Increment(command.args[0], delta);
      if (!result.ok()) return Error(result.status());
      const std::string event_value = std::to_string(delta);
      const auto status = Publish(command.type, command, command.args[0], event_value, context);
      if (!status.ok()) {
        if (status.code() == StatusCode::kInternal) return Error(status);
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
      std::size_t response_bytes = 32;
      for (const auto& key : command.args) {
        auto result = engine_->Get(key);
        if (result.ok()) {
          if (result.value().size() + 16U >
              context.response_budget - std::min(context.response_budget, response_bytes))
            return Invalid("BUSY response exceeds connection limit");
          response_bytes += result.value().size() + 16U;
          output.array.push_back(Value(std::move(result).value()));
        } else if (result.status().code() == StatusCode::kNotFound) {
          if (16U > context.response_budget - std::min(context.response_budget, response_bytes))
            return Invalid("BUSY response exceeds connection limit");
          response_bytes += 16U;
          output.array.push_back({true, {}, {}, false, true, {}});
        } else {
          return Error(result.status());
        }
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
      if (arity == 0) return Invalid("syntax error");
      {
        std::string subcommand = command.args[0];
        for (char& character : subcommand)
          character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        if (arity == 2 && subcommand == "SETNAME") return Success();
        if (arity == 3 && subcommand == "SETINFO") return Success();
        if (arity == 2 && subcommand == "GETNAME") {
          if (command.args[1].empty()) return {true, {}, {}, false, true, {}, false};
          return Value(command.args[1]);
        }
      }
      return Invalid("syntax error");
    case CommandType::kInfo:
      if (arity > 1) return Invalid("INVALID_ARGUMENT expected optional section");
      {
        std::string section = arity == 1 ? command.args[0] : "default";
        for (char& character : section)
          character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        if (section != "server" && section != "clients" && section != "persistence" &&
            section != "all" && section != "default")
          return Value({});
        if (section == "clients") return Value("# Clients\r\nconnected_clients:1\r\n");
        if (section == "persistence")
          return Value(info_provider_ ? info_provider_()
                                      : "# Persistence\r\nrecovery_phase:complete\r\n");
      }
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
    case CommandType::kUnknown:
      return Invalid("UNSUPPORTED unknown command");
    case CommandType::kDelMany:
      return Invalid("UNSUPPORTED internal event command");
  }
  return Invalid("INTERNAL unknown command");
}

}  // namespace kvstore
