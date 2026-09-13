#pragma once

#include <mutex>
#include <span>
#include <vector>

#include "kvstore/kvcache/chunk_registry.hpp"
#include "kvstore/kvcache/match_index.hpp"
#include "kvstore_integration_v1.pb.h"

namespace kvstore::integration {
inline constexpr std::uint16_t kMajor = 1, kMinor = 0;
struct Frame {
  std::uint8_t operation{};
  std::uint16_t major{};
  std::uint16_t minor{};
  Bytes payload;
};
class Codec {
 public:
  static constexpr std::size_t kHeader = 13;
  static constexpr std::size_t kMaxFrame = 16U * 1024U * 1024U;
  [[nodiscard]] static Result<Bytes> Encode(const Frame&);
  [[nodiscard]] static Result<Frame> Decode(ByteView);
  // Total buffered input per call is bounded, including coalesced frames.
  [[nodiscard]] static Result<std::vector<Frame>> Feed(Bytes&, ByteView);
  [[nodiscard]] static Result<Bytes> EncodeRequest(const v1::Request&);
  [[nodiscard]] static Result<v1::Request> DecodeRequest(ByteView);
  [[nodiscard]] static Result<Bytes> EncodeResponse(const v1::Response&);
  [[nodiscard]] static Result<v1::Response> DecodeResponse(ByteView);
};
[[nodiscard]] v1::TensorManifest ToProto(const kvcache::TensorManifest&);
enum class ManifestUse { kPublication, kQuery };
[[nodiscard]] Result<kvcache::TensorManifest> FromProto(
    const v1::TensorManifest&, ManifestUse use = ManifestUse::kPublication);

// Calls are serialized. Registry and shared index must outlive all sessions.
// The transport binds an authenticated tenant/model before constructing a session.
class Session {
 public:
  Session(kvcache::ChunkRegistry& registry, kvcache::MatchIndex& index, std::string tenant,
          std::string model);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Session(Session&&) = delete;
  Session& operator=(Session&&) = delete;
  [[nodiscard]] v1::Response Dispatch(const v1::Request&);
  [[nodiscard]] Result<Bytes> Exchange(ByteView);
  void Disconnect() noexcept;

 private:
  struct Reservation {
    std::uint64_t id;
    kvcache::TensorManifest manifest;
  };
  struct Lease {
    std::uint64_t id;
    kvcache::ResidentHandle handle;
  };
  [[nodiscard]] Status Execute(const v1::Request&, v1::Response&);
  [[nodiscard]] Status Pin(kvcache::ResidentHandle, v1::Response&);
  [[nodiscard]] Status PrepareLease(const kvcache::TensorManifest&, v1::Response&);
  kvcache::ChunkRegistry& registry_;
  kvcache::MatchIndex& index_;
  const std::string tenant_, model_;
  std::mutex mutex_;
  bool negotiated_{};
  bool closed_{};
  std::vector<Reservation> reservations_;
  std::vector<Lease> leases_;
  bool fail_tracking_{};
  friend class SessionTestPeer;
};
}  // namespace kvstore::integration
