#include "kvstore/replication/handshake.hpp"

#include <limits>
#include <sstream>
#include <unordered_set>

namespace kvstore {
namespace {

constexpr std::string_view kMagic = "KVRP";
constexpr std::size_t kHeaderBytes = 12;

void Put16(std::string& output, std::uint16_t value) {
  output.push_back(static_cast<char>((value >> 8U) & 0xffU));
  output.push_back(static_cast<char>(value & 0xffU));
}

bool Get16(std::string_view input, std::size_t& position, std::uint16_t& value) {
  if (input.size() - position < 2U) return false;
  const auto high = static_cast<std::uint16_t>(static_cast<unsigned char>(input[position]));
  const auto low = static_cast<std::uint16_t>(static_cast<unsigned char>(input[position + 1U]));
  value = static_cast<std::uint16_t>(static_cast<std::uint16_t>(high << 8U) | low);
  position += 2U;
  return true;
}

}  // namespace

Result<std::string> ReplicationHandshake::Encode(const ReplicationHello& hello) {
  if (hello.node_id.empty() || hello.role.empty() || hello.node_id.size() > 1024U ||
      hello.role.size() > 32U || hello.capabilities.size() > 1024U)
    return Status{StatusCode::kInvalidArgument, "invalid replication hello"};
  const auto payload_size = hello.node_id.size() + hello.role.size() + hello.capabilities.size();
  if (payload_size > std::numeric_limits<std::uint16_t>::max() ||
      kHeaderBytes + payload_size > kMaxFrameBytes)
    return Status{StatusCode::kLimitExceeded, "replication hello is too large"};
  std::string output(kMagic);
  Put16(output, ReplicationHello::kVersion);
  Put16(output, static_cast<std::uint16_t>(payload_size));
  Put16(output, static_cast<std::uint16_t>(hello.node_id.size()));
  Put16(output, static_cast<std::uint16_t>(hello.role.size()));
  output.append(hello.node_id);
  output.append(hello.role);
  output.append(hello.capabilities);
  return output;
}

Result<std::optional<ReplicationHello>> ReplicationHandshake::Decode(std::string& input) {
  if (input.size() < kHeaderBytes) return std::optional<ReplicationHello>{};
  if (input.compare(0, kMagic.size(), kMagic) != 0)
    return Status{StatusCode::kInvalidArgument, "replication hello magic mismatch"};
  std::size_t position = kMagic.size();
  std::uint16_t version = 0;
  std::uint16_t payload_size = 0;
  std::uint16_t node_size = 0;
  std::uint16_t role_size = 0;
  if (!Get16(input, position, version) || !Get16(input, position, payload_size) ||
      !Get16(input, position, node_size) || !Get16(input, position, role_size))
    return std::optional<ReplicationHello>{};
  if (payload_size > kMaxFrameBytes - kHeaderBytes ||
      static_cast<std::size_t>(node_size) + role_size > payload_size)
    return Status{StatusCode::kLimitExceeded, "invalid replication hello length"};
  const auto frame_size = kHeaderBytes + static_cast<std::size_t>(payload_size);
  if (frame_size > kMaxFrameBytes) return Status{StatusCode::kLimitExceeded, "hello frame limit"};
  if (input.size() < frame_size) return std::optional<ReplicationHello>{};
  if (version != ReplicationHello::kVersion)
    return Status{StatusCode::kUnsupported, "replication hello version mismatch"};
  if (node_size == 0 || role_size == 0)
    return Status{StatusCode::kInvalidArgument, "replication hello identity is empty"};
  const auto node_begin = kHeaderBytes;
  const auto role_begin = node_begin + node_size;
  const auto capability_begin = role_begin + role_size;
  const auto capability_size = static_cast<std::size_t>(payload_size) - node_size - role_size;
  ReplicationHello hello{std::string(input.substr(node_begin, node_size)),
                         std::string(input.substr(role_begin, role_size)),
                         std::string(input.substr(capability_begin, capability_size))};
  input.erase(0, frame_size);
  return std::optional<ReplicationHello>(std::move(hello));
}

ReplicationConnection::ReplicationConnection(std::chrono::steady_clock::time_point accepted_at,
                                             std::chrono::milliseconds handshake_timeout,
                                             std::string local_role,
                                             std::string required_capabilities)
    : accepted_at_(accepted_at),
      handshake_timeout_(handshake_timeout),
      local_role_(std::move(local_role)),
      required_capabilities_(std::move(required_capabilities)) {}

Status ReplicationConnection::Receive(std::string_view bytes,
                                      std::chrono::steady_clock::time_point now) {
  if (state_ != ReplicationConnectionState::kAccepted &&
      state_ != ReplicationConnectionState::kHandshake)
    return {StatusCode::kInvalidArgument, "replication handshake is not expected"};
  if (peer_.has_value())
    return {StatusCode::kInvalidArgument, "replication handshake is already complete"};
  if (bytes.size() > ReplicationHandshake::kMaxFrameBytes -
                         std::min(input_.size(), ReplicationHandshake::kMaxFrameBytes))
    return {StatusCode::kLimitExceeded, "replication handshake buffer limit"};
  input_.append(bytes);
  state_ = ReplicationConnectionState::kHandshake;
  auto decoded = ReplicationHandshake::Decode(input_);
  if (!decoded.ok()) {
    state_ = ReplicationConnectionState::kClosing;
    return decoded.status();
  }
  if (!decoded.value().has_value()) return Status::Ok();
  peer_ = std::move(decoded.value().value());
  if (peer_->role != "primary" && peer_->role != "replica") {
    state_ = ReplicationConnectionState::kClosing;
    return {StatusCode::kInvalidArgument, "unknown replication role"};
  }
  if (local_role_ != "dual" &&
      ((local_role_ == "replica" && peer_->role != "primary") ||
       (local_role_ == "primary" && peer_->role != "replica"))) {
    state_ = ReplicationConnectionState::kClosing;
    return {StatusCode::kInvalidArgument, "replication peer role is incompatible"};
  }
  std::unordered_set<std::string> capabilities;
  std::stringstream capability_stream(peer_->capabilities);
  std::string capability;
  while (std::getline(capability_stream, capability, ',')) capabilities.insert(capability);
  std::stringstream required_stream(required_capabilities_);
  while (std::getline(required_stream, capability, ',')) {
    if (!capabilities.contains(capability)) {
      state_ = ReplicationConnectionState::kClosing;
      return {StatusCode::kUnsupported, "replication capability is missing"};
    }
  }
  if (now - accepted_at_ > handshake_timeout_) {
    state_ = ReplicationConnectionState::kClosing;
    return {StatusCode::kDeadlineExceeded, "replication handshake timeout"};
  }
  state_ = ReplicationConnectionState::kHandshake;
  return Status::Ok();
}

Status ReplicationConnection::CheckTimeout(std::chrono::steady_clock::time_point now) const {
  if ((state_ == ReplicationConnectionState::kAccepted ||
       state_ == ReplicationConnectionState::kHandshake) &&
      now - accepted_at_ >= handshake_timeout_)
    return {StatusCode::kDeadlineExceeded, "replication handshake timeout"};
  return Status::Ok();
}

Status ReplicationConnection::BeginFullSync() {
  if (state_ != ReplicationConnectionState::kHandshake &&
      state_ != ReplicationConnectionState::kOnline &&
      state_ != ReplicationConnectionState::kCatchUp &&
      state_ != ReplicationConnectionState::kFullSync)
    return {StatusCode::kInvalidArgument, "full sync requires replication connection"};
  state_ = ReplicationConnectionState::kFullSync;
  return Status::Ok();
}

Status ReplicationConnection::BeginCatchUp() {
  if (state_ != ReplicationConnectionState::kFullSync)
    return {StatusCode::kInvalidArgument, "catch-up requires full sync"};
  state_ = ReplicationConnectionState::kCatchUp;
  return Status::Ok();
}

Status ReplicationConnection::MarkOnline() {
  if (state_ != ReplicationConnectionState::kCatchUp &&
      !(state_ == ReplicationConnectionState::kHandshake && local_role_ == "primary"))
    return {StatusCode::kInvalidArgument, "online requires replication setup"};
  state_ = ReplicationConnectionState::kOnline;
  return Status::Ok();
}

}  // namespace kvstore
