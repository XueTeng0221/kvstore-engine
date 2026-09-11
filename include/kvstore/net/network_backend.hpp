#pragma once

#include "kvstore/common/status.hpp"

namespace kvstore {

class INetworkBackend {
 public:
  virtual ~INetworkBackend() = default;
  [[nodiscard]] virtual Status Run() = 0;
  virtual void Stop() noexcept = 0;
};

}  // namespace kvstore
