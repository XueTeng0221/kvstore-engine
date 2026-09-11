#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stop_token>
#include <string>
#include <vector>

namespace kvstore {

enum class CommandType {
  kSet,
  kGet,
  kDel,
  kDelMany,
  kMod,
  kExist,
  kSave,
  kLoad,
  kIncr,
  kDecr,
  kIncrBy,
  kDecrBy,
  kMget,
  kPing,
  kEcho,
  kClient,
  kInfo,
  kUnknown
};

struct RequestContext {
  std::uint64_t request_id{};
  std::uint64_t connection_id{};
  std::chrono::steady_clock::time_point deadline{std::chrono::steady_clock::time_point::max()};
  std::stop_token stop;
  std::size_t response_budget{std::numeric_limits<std::size_t>::max()};
};
enum class WriteSource { kClient, kAofReplay, kFullSync, kIncrementalSync };

struct Command {
  CommandType type;
  std::vector<std::string> args;
  WriteSource source{WriteSource::kClient};
  bool redis_compat{false};
};

struct CommandResponse {
  bool ok{false};
  std::string value;
  std::string error;
  bool integer{false};
  bool null_value{false};
  std::vector<CommandResponse> array;
  bool simple_string{false};
};

}  // namespace kvstore
