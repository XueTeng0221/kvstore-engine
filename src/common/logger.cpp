#include "kvstore/common/logger.hpp"

#include <nlohmann/json.hpp>

namespace kvstore {
namespace {

std::string_view LevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug:
      return "debug";
    case LogLevel::kInfo:
      return "info";
    case LogLevel::kWarning:
      return "warning";
    case LogLevel::kError:
      return "error";
  }
  return "error";
}

}  // namespace

void Logger::Log(LogLevel level, std::string_view message, const LogContext& context) {
  if (level < minimum_) return;
  nlohmann::json record{
      {"level", LevelName(level)},        {"message", message},
      {"request_id", context.request_id}, {"connection_id", context.connection_id},
      {"node_id", context.node_id},       {"event_offset", context.event_offset}};
  std::scoped_lock lock(mutex_);
  output_ << record.dump() << '\n';
  output_.flush();
}

}  // namespace kvstore
