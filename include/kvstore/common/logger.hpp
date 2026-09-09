#pragma once

#include <cstdint>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>

namespace kvstore {

enum class LogLevel { kDebug, kInfo, kWarning, kError };

struct LogContext {
  std::string request_id;
  std::uint64_t connection_id{0};
  std::string node_id;
  std::uint64_t event_offset{0};
};

class Logger {
 public:
  explicit Logger(std::ostream& output, LogLevel minimum = LogLevel::kInfo)
      : output_(output), minimum_(minimum) {}
  void Log(LogLevel level, std::string_view message, const LogContext& context = {});

 private:
  std::ostream& output_;
  LogLevel minimum_;
  std::mutex mutex_;
};

}  // namespace kvstore
