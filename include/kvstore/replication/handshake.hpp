#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "kvstore/common/result.hpp"

namespace kvstore {

struct ReplicationHello {
  static constexpr std::uint16_t kVersion = 1;
  std::string node_id;
  std::string role;
  std::string capabilities;
};

class ReplicationHandshake {
 public:
  static constexpr std::size_t kMaxFrameBytes = 4096;
  [[nodiscard]] static Result<std::string> Encode(const ReplicationHello& hello);
  [[nodiscard]] static Result<std::optional<ReplicationHello>> Decode(std::string& input);
};

enum class ReplicationConnectionState {
  kAccepted,
  kHandshake,
  kFullSync,
  kCatchUp,
  kOnline,
  kClosing,
};

class ReplicationConnection {
 public:
  ReplicationConnection(
      std::chrono::steady_clock::time_point accepted_at,
      std::chrono::milliseconds handshake_timeout, std::string local_role = "replica",
      std::string required_capabilities = "snapshot,backlog,ack,heartbeat,full-sync-request");

  [[nodiscard]] Status Receive(std::string_view bytes, std::chrono::steady_clock::time_point now);
  [[nodiscard]] Status CheckTimeout(std::chrono::steady_clock::time_point now) const;
  [[nodiscard]] ReplicationConnectionState state() const noexcept { return state_; }
  [[nodiscard]] const std::optional<ReplicationHello>& peer() const noexcept { return peer_; }
  [[nodiscard]] std::string TakePendingBytes() { return std::exchange(input_, {}); }
  [[nodiscard]] Status BeginFullSync();
  [[nodiscard]] Status BeginCatchUp();
  [[nodiscard]] Status MarkOnline();
  void Close() noexcept { state_ = ReplicationConnectionState::kClosing; }

 private:
  ReplicationConnectionState state_{ReplicationConnectionState::kAccepted};
  std::chrono::steady_clock::time_point accepted_at_;
  std::chrono::milliseconds handshake_timeout_;
  std::string input_;
  std::optional<ReplicationHello> peer_;
  std::string local_role_;
  std::string required_capabilities_;
};

}  // namespace kvstore
