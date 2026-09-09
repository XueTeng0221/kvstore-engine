#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kvstore {

enum class CommandType {
  kSet,
  kGet,
  kDel,
  kMod,
  kExist,
  kSave,
  kLoad,
  kIncr,
  kDecr,
  kMget,
  kPing,
  kEcho,
  kClient,
  kInfo
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
