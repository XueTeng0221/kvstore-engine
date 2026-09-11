#pragma once

#include <cstdint>
#include <filesystem>

#include "kvstore/engine/engine.hpp"

namespace kvstore {

struct RecoveryPoint {
  std::uint64_t offset{};
  std::uint64_t event_id{};
  operator std::uint64_t() const noexcept { return offset; }
  friend bool operator==(const RecoveryPoint&, const RecoveryPoint&) = default;
};

class Snapshot {
 public:
  static constexpr std::uint32_t kVersion = 1;
  [[nodiscard]] static Status Save(const std::filesystem::path& directory, const IEngine& engine,
                                   std::uint64_t last_offset, std::uint64_t last_event_id = 0,
                                   bool io_uring_write = false, bool allow_sync_fallback = false);
  [[nodiscard]] static Result<RecoveryPoint> LoadWithMetadata(
      const std::filesystem::path& directory, IEngine& engine);
  [[nodiscard]] static Result<std::uint64_t> Load(const std::filesystem::path& directory,
                                                  IEngine& engine);
};

}  // namespace kvstore
