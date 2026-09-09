#pragma once

#include <cstdint>
#include <filesystem>

#include "kvstore/engine/engine.hpp"

namespace kvstore {

class Snapshot {
 public:
  static constexpr std::uint32_t kVersion = 1;
  [[nodiscard]] static Status Save(const std::filesystem::path& directory, const IEngine& engine,
                                   std::uint64_t last_offset);
  [[nodiscard]] static Result<std::uint64_t> Load(const std::filesystem::path& directory,
                                                  IEngine& engine);
};

}  // namespace kvstore
