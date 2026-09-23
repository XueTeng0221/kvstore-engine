#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "kvstore/command/dispatcher.hpp"
#include "kvstore/persistence/snapshot.hpp"
#include "kvstore/replication/sync.hpp"

namespace kvstore {

enum class ReplicationFrameType : std::uint8_t {
  kSnapshot = 1,
  kEvents = 2,
  kAck = 3,
  kHeartbeat = 4,
  kFullSyncRequest = 5,
  kChunk = 6,
};

class ReplicationChunkGenerator {
 public:
  [[nodiscard]] static Result<ReplicationChunkGenerator> Snapshot(
      RecoveryPoint point, std::vector<Entry> entries, std::uint64_t transfer_id,
      std::size_t chunk_payload_bytes = 64U * 1024U);
  [[nodiscard]] static Result<ReplicationChunkGenerator> Events(
      std::vector<WriteEvent> events, std::uint64_t transfer_id,
      std::size_t chunk_payload_bytes = 64U * 1024U);
  [[nodiscard]] Result<std::optional<std::string>> Next();
  [[nodiscard]] std::uint64_t total_size() const { return total_size_; }
  [[nodiscard]] std::uint32_t chunk_count() const { return chunk_count_; }

 private:
  ReplicationChunkGenerator(ReplicationFrameType type, RecoveryPoint point,
                            std::vector<Entry> entries, std::vector<WriteEvent> events,
                            std::uint64_t transfer_id, std::size_t chunk_payload_bytes,
                            std::uint64_t total_size, std::uint32_t chunk_count);
  [[nodiscard]] std::string PayloadSlice(std::uint64_t begin, std::size_t length) const;

  ReplicationFrameType type_;
  RecoveryPoint point_;
  std::variant<std::vector<Entry>, std::vector<WriteEvent>> source_;
  std::uint64_t transfer_id_;
  std::size_t chunk_payload_bytes_;
  std::uint64_t total_size_;
  std::uint32_t chunk_count_;
  std::uint32_t next_index_{};
};

struct ReplicationFrame {
  ReplicationFrameType type{};
  std::string payload;
  std::uint64_t transfer_id{};
  std::uint32_t chunk_index{};
  std::uint32_t chunk_count{};
  ReplicationFrameType chunk_type{};
  std::uint64_t chunk_total_size{};
};

class ReplicationFrameCodec {
 public:
  static constexpr std::size_t kMaxFrameBytes = 64U << 20U;
  static constexpr std::size_t kMaxTransferBytes = 256U << 20U;
  static constexpr std::size_t kMaxEvents = 1024;

  [[nodiscard]] static Result<std::string> EncodeSnapshot(RecoveryPoint point,
                                                          const std::vector<Entry>& entries);
  [[nodiscard]] static Result<std::string> EncodeEvents(const std::vector<WriteEvent>& events);
  [[nodiscard]] static Result<std::string> EncodeAck(ReplicationAck ack);
  [[nodiscard]] static Result<std::string> EncodeHeartbeat(std::uint64_t offset);
  [[nodiscard]] static Result<std::string> EncodeFullSyncRequest(std::uint64_t next_offset);
  [[nodiscard]] static Result<std::optional<ReplicationFrame>> Decode(std::string& input);
  [[nodiscard]] static Result<std::pair<RecoveryPoint, std::vector<Entry>>> DecodeSnapshot(
      const ReplicationFrame& frame);
  [[nodiscard]] static Result<std::vector<WriteEvent>> DecodeEvents(const ReplicationFrame& frame);
  [[nodiscard]] static Result<ReplicationAck> DecodeAck(const ReplicationFrame& frame);
  [[nodiscard]] static Result<std::uint64_t> DecodeHeartbeat(const ReplicationFrame& frame);
  [[nodiscard]] static Result<std::uint64_t> DecodeFullSyncRequest(const ReplicationFrame& frame);
  [[nodiscard]] static Result<std::string> ReassembleChunks(
      const std::vector<ReplicationFrame>& chunks);
};

}  // namespace kvstore
