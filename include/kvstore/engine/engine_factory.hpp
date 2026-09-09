#pragma once

#include <memory>
#include <string_view>

#include "kvstore/common/result.hpp"
#include "kvstore/config/config.hpp"
#include "kvstore/engine/engine.hpp"

namespace kvstore {

[[nodiscard]] Result<std::unique_ptr<IEngine>> CreateEngine(const EngineConfig& config);
[[nodiscard]] Result<std::unique_ptr<IEngine>> CreateEngine(std::string_view type,
                                                            std::size_t capacity);

}  // namespace kvstore
