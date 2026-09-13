#pragma once

#include <memory>
#include <stop_token>

#include "kvstore/integration/protocol.hpp"

namespace kvstore::integration::vllm {

inline constexpr std::string_view kVersion = "0.29.0";
inline constexpr std::string_view kModel = "Qwen/Qwen2.5-0.5B";
inline constexpr std::string_view kFormat = "vllm-0.29.0-kvconnector-v1-planar";
inline constexpr std::uint32_t kBlockTokens = 16;
inline constexpr std::uint32_t kMaxTokens = 512;

// Supplied by the embedding application's validated configuration, not CLI overrides.
struct Options {
  std::string tenant;
  std::string model_revision;
  std::string tokenizer_revision;
  std::string framework_version{kVersion};
};

struct Request {
  std::string id;
  std::vector<std::uint32_t> tokens;
  std::uint64_t deadline_unix_ms{};
  std::stop_token stop;
};

// IDs are destination allocator slots, never server leases or source block IDs.
struct BlockTable {
  std::vector<std::uint64_t> ids;
  std::uint64_t capacity{};
  std::uint64_t locally_computed_tokens{};
};

struct Hit {
  std::string request_id;
  kvcache::TensorManifest manifest;
  Bytes payload;
  std::vector<std::uint64_t> destination_blocks;
};

class HitInjector {
 public:
  virtual ~HitInjector() = default;
  // All-or-nothing: success means copies completed and blocks are usable. Failure
  // must leave no usable blocks. Do not retain references after returning.
  [[nodiscard]] virtual Status Install(const Hit&, const Request&) = 0;
};

class Transport {
 public:
  virtual ~Transport() = default;
  [[nodiscard]] virtual Result<Bytes> Exchange(ByteView) = 0;
  virtual void Disconnect() noexcept = 0;
};

class SessionTransport final : public Transport {
 public:
  SessionTransport(kvcache::ChunkRegistry& registry, kvcache::MatchIndex& index,
                   const std::string& tenant)
      : session_(registry, index, tenant, std::string(kModel)) {}
  [[nodiscard]] Result<Bytes> Exchange(ByteView bytes) override { return session_.Exchange(bytes); }
  void Disconnect() noexcept override { session_.Disconnect(); }

 private:
  Session session_;
};

struct PrefillResult {
  Status status;
  std::uint64_t hit_tokens{};
  // Recompute [recompute_begin, total_tokens). No payload on a failed lookup.
  std::uint64_t recompute_begin{};
  std::uint64_t total_tokens{};
};

enum class PublicationState {
  kIdle,
  kAwaitingCopy,
  kReserve,
  kPut,
  kCommit,
  kPublished,
  kCancelled,
  kFailed
};

// Single executor/thread affinity. Only Request::stop may be signalled concurrently.
// Owns its transport; registry/index must outlive SessionTransport. No detached work.
class Adapter {
 public:
  Adapter(std::unique_ptr<Transport> transport, Options options);
  ~Adapter();
  Adapter(const Adapter&) = delete;
  Adapter& operator=(const Adapter&) = delete;

  [[nodiscard]] Status Connect();
  [[nodiscard]] Result<kvcache::TensorManifest> Manifest(const Request&) const;
  [[nodiscard]] PrefillResult Prefill(const Request&, bool exact, const BlockTable&, HitInjector&);
  [[nodiscard]] Status BeginPublication(const Request&);
  // Called only after D2H completion; copies into bounded adapter-owned storage.
  [[nodiscard]] Status HostCopyReady(std::uint64_t generation, ByteView);
  // One RESERVE, PUT, or COMMIT per call. AwaitingCopy never blocks on an event.
  [[nodiscard]] Status AdvancePublication();
  [[nodiscard]] Status CancelPublication();
  [[nodiscard]] PublicationState publication_state() const noexcept { return state_; }
  [[nodiscard]] std::size_t staging_bytes() const noexcept { return payload_.size(); }
  [[nodiscard]] std::uint64_t copy_generation() const noexcept { return generation_; }

 private:
  [[nodiscard]] v1::Request Message(v1::Operation, const Request&) const;
  [[nodiscard]] Result<v1::Response> Call(const v1::Request&);
  [[nodiscard]] Status Cleanup(v1::Operation, std::uint64_t);
  [[nodiscard]] Status FailPublication(Status);
  void Close() noexcept;
  std::unique_ptr<Transport> transport_;
  Options options_;
  bool connected_{};
  bool closed_{};
  PublicationState state_{PublicationState::kIdle};
  Request publication_;
  kvcache::TensorManifest manifest_;
  Bytes payload_;
  std::uint64_t reservation_{};
  std::uint32_t next_chunk_{};
  std::uint64_t generation_{};
};

}  // namespace kvstore::integration::vllm
