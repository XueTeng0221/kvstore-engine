#include "kvstore/replication/frame.hpp"

#include <limits>
#include <span>

#include "kvstore/common/crc32.hpp"

namespace kvstore {
namespace {

constexpr std::string_view kMagic = "KVRF";
constexpr std::size_t kHeaderBytes = 13;

void Put32(std::string& out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>(value >> shift));
}
void Put64(std::string& out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<char>(value >> shift));
}
bool Get32(std::string_view in, std::size_t& pos, std::uint32_t& value) {
  if (in.size() - pos < 4U) return false;
  value = 0;
  for (int index = 0; index < 4; ++index)
    value = (value << 8U) | static_cast<unsigned char>(in[pos++]);
  return true;
}
bool Get64(std::string_view in, std::size_t& pos, std::uint64_t& value) {
  if (in.size() - pos < 8U) return false;
  value = 0;
  for (int index = 0; index < 8; ++index)
    value = (value << 8U) | static_cast<unsigned char>(in[pos++]);
  return true;
}
Result<std::string> Wrap(ReplicationFrameType type, std::string payload) {
  if (payload.size() > ReplicationFrameCodec::kMaxFrameBytes - kHeaderBytes)
    return Status{StatusCode::kLimitExceeded, "replication frame is too large"};
  std::string output(kMagic);
  output.push_back(static_cast<char>(type));
  Put32(output, static_cast<std::uint32_t>(payload.size()));
  Put32(output, Crc32(std::as_bytes(std::span(payload.data(), payload.size()))));
  output.append(payload);
  return output;
}
void PutString(std::string& out, std::string_view value);
Result<std::string> EncodeSnapshotPayload(RecoveryPoint point, const std::vector<Entry>& entries) {
  if (entries.size() > std::numeric_limits<std::uint32_t>::max())
    return Status{StatusCode::kLimitExceeded, "too many snapshot entries"};
  std::string payload;
  Put64(payload, point.offset);
  Put64(payload, point.event_id);
  Put32(payload, static_cast<std::uint32_t>(entries.size()));
  for (const auto& entry : entries) {
    if (entry.key.size() > std::numeric_limits<std::uint32_t>::max() ||
        entry.value.size() > std::numeric_limits<std::uint32_t>::max())
      return Status{StatusCode::kLimitExceeded, "snapshot record is too large"};
    PutString(payload, entry.key);
    PutString(payload, entry.value);
  }
  return payload;
}
Result<std::string> EncodeEventsPayload(const std::vector<WriteEvent>& events) {
  if (events.empty() || events.size() > ReplicationFrameCodec::kMaxEvents)
    return Status{StatusCode::kInvalidArgument, "invalid replication event batch"};
  std::string payload;
  Put32(payload, static_cast<std::uint32_t>(events.size()));
  for (const auto& event : events) {
    if (event.key.size() > std::numeric_limits<std::uint32_t>::max() ||
        event.value.size() > std::numeric_limits<std::uint32_t>::max() ||
        event.origin_node.size() > std::numeric_limits<std::uint32_t>::max())
      return Status{StatusCode::kLimitExceeded, "replication event field is too large"};
    Put64(payload, event.offset);
    Put64(payload, event.event_id);
    payload.push_back(static_cast<char>(event.source));
    payload.push_back(static_cast<char>(event.type));
    Put64(payload, event.timestamp_ms);
    PutString(payload, event.key);
    PutString(payload, event.value);
    PutString(payload, event.origin_node);
    Put32(payload, event.checksum);
  }
  return payload;
}
Result<std::string> ReadString(std::string_view in, std::size_t& pos) {
  std::uint32_t length = 0;
  if (!Get32(in, pos, length) || length > in.size() - pos)
    return Status{StatusCode::kCorruption, "replication string bounds"};
  std::string value(in.substr(pos, length));
  pos += length;
  return value;
}
void PutString(std::string& out, std::string_view value) {
  Put32(out, static_cast<std::uint32_t>(value.size()));
  out.append(value);
}

bool AddSize(std::uint64_t& size, std::uint64_t amount) {
  if (amount > std::numeric_limits<std::uint64_t>::max() - size) return false;
  size += amount;
  return size <= ReplicationFrameCodec::kMaxTransferBytes;
}

std::uint64_t SnapshotSize(const std::vector<Entry>& entries) {
  std::uint64_t size = 20;
  if (entries.size() > std::numeric_limits<std::uint32_t>::max()) return 0;
  for (const auto& entry : entries) {
    if (entry.key.size() > std::numeric_limits<std::uint32_t>::max() ||
        entry.value.size() > std::numeric_limits<std::uint32_t>::max() ||
        !AddSize(size, 8ULL + entry.key.size() + entry.value.size()))
      return 0;
  }
  return size;
}

std::uint64_t EventsSize(const std::vector<WriteEvent>& events) {
  std::uint64_t size = 4;
  if (events.empty() || events.size() > ReplicationFrameCodec::kMaxEvents) return 0;
  for (const auto& event : events) {
    if (event.key.size() > std::numeric_limits<std::uint32_t>::max() ||
        event.value.size() > std::numeric_limits<std::uint32_t>::max() ||
        event.origin_node.size() > std::numeric_limits<std::uint32_t>::max() ||
        !AddSize(size, 42ULL + event.key.size() + event.value.size() + event.origin_node.size()))
      return 0;
  }
  return size;
}

}  // namespace

ReplicationChunkGenerator::ReplicationChunkGenerator(
    ReplicationFrameType type, RecoveryPoint point, std::vector<Entry> entries,
    std::vector<WriteEvent> events, std::uint64_t transfer_id, std::size_t chunk_payload_bytes,
    std::uint64_t total_size, std::uint32_t chunk_count)
    : type_(type),
      point_(point),
      source_(type == ReplicationFrameType::kSnapshot
                  ? std::variant<std::vector<Entry>, std::vector<WriteEvent>>(std::move(entries))
                  : std::variant<std::vector<Entry>, std::vector<WriteEvent>>(std::move(events))),
      transfer_id_(transfer_id),
      chunk_payload_bytes_(chunk_payload_bytes),
      total_size_(total_size),
      chunk_count_(chunk_count) {}

Result<ReplicationChunkGenerator> ReplicationChunkGenerator::Snapshot(
    RecoveryPoint point, std::vector<Entry> entries, std::uint64_t transfer_id,
    std::size_t chunk_payload_bytes) {
  const auto size = SnapshotSize(entries);
  if (size == 0 || chunk_payload_bytes == 0 ||
      chunk_payload_bytes > ReplicationFrameCodec::kMaxFrameBytes - 29U)
    return Status{StatusCode::kLimitExceeded, "invalid snapshot chunk bounds"};
  const auto count = 1U + (size - 1U) / chunk_payload_bytes;
  if (count > 16'384U) return Status{StatusCode::kLimitExceeded, "too many replication chunks"};
  return ReplicationChunkGenerator(ReplicationFrameType::kSnapshot, point, std::move(entries), {},
                                   transfer_id, chunk_payload_bytes, size,
                                   static_cast<std::uint32_t>(count));
}

Result<ReplicationChunkGenerator> ReplicationChunkGenerator::Events(
    std::vector<WriteEvent> events, std::uint64_t transfer_id, std::size_t chunk_payload_bytes) {
  const auto size = EventsSize(events);
  if (size == 0 || chunk_payload_bytes == 0 ||
      chunk_payload_bytes > ReplicationFrameCodec::kMaxFrameBytes - 29U)
    return Status{StatusCode::kLimitExceeded, "invalid event chunk bounds"};
  const auto count = 1U + (size - 1U) / chunk_payload_bytes;
  if (count > 16'384U) return Status{StatusCode::kLimitExceeded, "too many replication chunks"};
  return ReplicationChunkGenerator(ReplicationFrameType::kEvents, {}, {}, std::move(events),
                                   transfer_id, chunk_payload_bytes, size,
                                   static_cast<std::uint32_t>(count));
}

std::string ReplicationChunkGenerator::PayloadSlice(std::uint64_t begin, std::size_t length) const {
  std::string output;
  output.reserve(length);
  const auto end = begin + length;
  std::uint64_t position = 0;
  auto append = [&](std::string_view bytes) {
    const auto field_end = position + bytes.size();
    const auto from = std::max(position, begin);
    const auto to = std::min(field_end, end);
    if (from < to)
      output.append(bytes.substr(static_cast<std::size_t>(from - position),
                                 static_cast<std::size_t>(to - from)));
    position = field_end;
  };
  auto number32 = [&](std::uint32_t value) {
    std::string b;
    Put32(b, value);
    append(b);
  };
  auto number64 = [&](std::uint64_t value) {
    std::string b;
    Put64(b, value);
    append(b);
  };
  if (type_ == ReplicationFrameType::kSnapshot) {
    number64(point_.offset);
    number64(point_.event_id);
    const auto& entries = std::get<std::vector<Entry>>(source_);
    number32(static_cast<std::uint32_t>(entries.size()));
    for (const auto& e : entries) {
      number32(static_cast<std::uint32_t>(e.key.size()));
      append(e.key);
      number32(static_cast<std::uint32_t>(e.value.size()));
      append(e.value);
    }
  } else {
    const auto& events = std::get<std::vector<WriteEvent>>(source_);
    number32(static_cast<std::uint32_t>(events.size()));
    for (const auto& e : events) {
      number64(e.offset);
      number64(e.event_id);
      const char source = static_cast<char>(e.source), type = static_cast<char>(e.type);
      append({&source, 1});
      append({&type, 1});
      number64(e.timestamp_ms);
      number32(static_cast<std::uint32_t>(e.key.size()));
      append(e.key);
      number32(static_cast<std::uint32_t>(e.value.size()));
      append(e.value);
      number32(static_cast<std::uint32_t>(e.origin_node.size()));
      append(e.origin_node);
      number32(e.checksum);
    }
  }
  return output;
}

Result<std::optional<std::string>> ReplicationChunkGenerator::Next() {
  if (next_index_ == chunk_count_) return std::optional<std::string>{};
  const auto begin = static_cast<std::uint64_t>(next_index_) * chunk_payload_bytes_;
  const auto length =
      static_cast<std::size_t>(std::min<std::uint64_t>(chunk_payload_bytes_, total_size_ - begin));
  std::string chunk;
  chunk.reserve(25U + length);
  chunk.push_back(static_cast<char>(type_));
  Put64(chunk, transfer_id_);
  Put32(chunk, next_index_);
  Put32(chunk, chunk_count_);
  Put64(chunk, total_size_);
  chunk.append(PayloadSlice(begin, length));
  auto frame = Wrap(ReplicationFrameType::kChunk, std::move(chunk));
  if (!frame.ok()) return frame.status();
  ++next_index_;
  return std::optional<std::string>(std::move(frame).value());
}

Result<std::string> ReplicationFrameCodec::EncodeSnapshot(RecoveryPoint point,
                                                          const std::vector<Entry>& entries) {
  auto payload = EncodeSnapshotPayload(point, entries);
  if (!payload.ok()) return payload.status();
  return Wrap(ReplicationFrameType::kSnapshot, std::move(payload).value());
}

Result<std::string> ReplicationFrameCodec::EncodeEvents(const std::vector<WriteEvent>& events) {
  auto payload = EncodeEventsPayload(events);
  if (!payload.ok()) return payload.status();
  return Wrap(ReplicationFrameType::kEvents, std::move(payload).value());
}

Result<std::string> ReplicationFrameCodec::EncodeAck(ReplicationAck ack) {
  std::string payload;
  Put64(payload, ack.offset);
  Put64(payload, ack.event_id);
  return Wrap(ReplicationFrameType::kAck, std::move(payload));
}

Result<std::string> ReplicationFrameCodec::EncodeHeartbeat(std::uint64_t offset) {
  std::string payload;
  Put64(payload, offset);
  return Wrap(ReplicationFrameType::kHeartbeat, std::move(payload));
}

Result<std::optional<ReplicationFrame>> ReplicationFrameCodec::Decode(std::string& input) {
  if (input.size() < kHeaderBytes) return std::optional<ReplicationFrame>{};
  if (input.compare(0, kMagic.size(), kMagic) != 0)
    return Status{StatusCode::kInvalidArgument, "replication frame magic mismatch"};
  const auto type = static_cast<ReplicationFrameType>(static_cast<unsigned char>(input[4]));
  std::size_t pos = 5;
  std::uint32_t length = 0;
  std::uint32_t checksum = 0;
  if (!Get32(input, pos, length) || !Get32(input, pos, checksum))
    return std::optional<ReplicationFrame>{};
  if (length > kMaxFrameBytes - kHeaderBytes)
    return Status{StatusCode::kLimitExceeded, "replication frame limit"};
  const auto frame_size = kHeaderBytes + static_cast<std::size_t>(length);
  if (input.size() < frame_size) return std::optional<ReplicationFrame>{};
  const std::string payload(input.substr(kHeaderBytes, length));
  if (Crc32(std::as_bytes(std::span(payload.data(), payload.size()))) != checksum)
    return Status{StatusCode::kCorruption, "replication frame checksum"};
  input.erase(0, frame_size);
  if (type == ReplicationFrameType::kChunk) {
    std::size_t chunk_pos = 0;
    if (payload.size() < 25U) return Status{StatusCode::kCorruption, "replication chunk header"};
    const auto chunk_type =
        static_cast<ReplicationFrameType>(static_cast<unsigned char>(payload[chunk_pos++]));
    std::uint64_t transfer_id = 0;
    std::uint32_t chunk_index = 0;
    std::uint32_t chunk_count = 0;
    std::uint64_t total_size = 0;
    if (!Get64(payload, chunk_pos, transfer_id) || !Get32(payload, chunk_pos, chunk_index) ||
        !Get32(payload, chunk_pos, chunk_count) || !Get64(payload, chunk_pos, total_size) ||
        chunk_count == 0 || chunk_count > 16'384U || chunk_index >= chunk_count ||
        total_size > ReplicationFrameCodec::kMaxTransferBytes)
      return Status{StatusCode::kCorruption, "replication chunk metadata"};
    return std::optional<ReplicationFrame>(
        ReplicationFrame{type, std::string(payload.substr(chunk_pos)), transfer_id, chunk_index,
                         chunk_count, chunk_type, total_size});
  }
  return std::optional<ReplicationFrame>(ReplicationFrame{type, payload});
}

Result<std::pair<RecoveryPoint, std::vector<Entry>>> ReplicationFrameCodec::DecodeSnapshot(
    const ReplicationFrame& frame) {
  if (frame.type != ReplicationFrameType::kSnapshot)
    return Status{StatusCode::kInvalidArgument, "not a snapshot frame"};
  std::size_t pos = 0;
  std::uint64_t offset = 0;
  std::uint64_t event_id = 0;
  std::uint32_t count = 0;
  if (!Get64(frame.payload, pos, offset) || !Get64(frame.payload, pos, event_id) ||
      !Get32(frame.payload, pos, count) || count > 2'000'000U ||
      count > (frame.payload.size() - pos) / 8U)
    return Status{StatusCode::kCorruption, "snapshot frame header"};
  std::vector<Entry> entries;
  entries.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    auto key = ReadString(frame.payload, pos);
    auto value = ReadString(frame.payload, pos);
    if (!key.ok() || !value.ok()) return Status{StatusCode::kCorruption, "snapshot frame record"};
    entries.push_back({std::move(key).value(), std::move(value).value()});
  }
  if (pos != frame.payload.size())
    return Status{StatusCode::kCorruption, "snapshot frame trailing bytes"};
  return std::pair<RecoveryPoint, std::vector<Entry>>{{offset, event_id}, std::move(entries)};
}

Result<std::vector<WriteEvent>> ReplicationFrameCodec::DecodeEvents(const ReplicationFrame& frame) {
  if (frame.type != ReplicationFrameType::kEvents)
    return Status{StatusCode::kInvalidArgument, "not an events frame"};
  std::size_t pos = 0;
  std::uint32_t count = 0;
  if (!Get32(frame.payload, pos, count) || count == 0 || count > kMaxEvents ||
      count > (frame.payload.size() - pos) / 26U)
    return Status{StatusCode::kCorruption, "events frame count"};
  std::vector<WriteEvent> events;
  events.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    std::uint64_t offset = 0;
    std::uint64_t event_id = 0;
    std::uint64_t timestamp = 0;
    if (!Get64(frame.payload, pos, offset) || !Get64(frame.payload, pos, event_id) ||
        pos >= frame.payload.size())
      return Status{StatusCode::kCorruption, "events frame header"};
    const auto source = static_cast<WriteSource>(static_cast<unsigned char>(frame.payload[pos++]));
    if (pos >= frame.payload.size()) return Status{StatusCode::kCorruption, "events frame type"};
    const auto command = static_cast<CommandType>(static_cast<unsigned char>(frame.payload[pos++]));
    if (!Get64(frame.payload, pos, timestamp))
      return Status{StatusCode::kCorruption, "events frame timestamp"};
    auto key = ReadString(frame.payload, pos);
    auto value = ReadString(frame.payload, pos);
    auto origin = ReadString(frame.payload, pos);
    std::uint32_t checksum = 0;
    if (!key.ok() || !value.ok() || !origin.ok() || !Get32(frame.payload, pos, checksum))
      return Status{StatusCode::kCorruption, "events frame record"};
    events.emplace_back(offset, event_id, source, command, std::move(key).value(),
                        std::move(value).value(), std::move(origin).value(), timestamp, checksum);
  }
  if (pos != frame.payload.size())
    return Status{StatusCode::kCorruption, "events frame trailing bytes"};
  return events;
}

Result<ReplicationAck> ReplicationFrameCodec::DecodeAck(const ReplicationFrame& frame) {
  if (frame.type != ReplicationFrameType::kAck || frame.payload.size() != 16U)
    return Status{StatusCode::kInvalidArgument, "not an ack frame"};
  std::size_t pos = 0;
  ReplicationAck ack;
  if (!Get64(frame.payload, pos, ack.offset) || !Get64(frame.payload, pos, ack.event_id))
    return Status{StatusCode::kCorruption, "ack frame"};
  return ack;
}

Result<std::uint64_t> ReplicationFrameCodec::DecodeHeartbeat(const ReplicationFrame& frame) {
  if (frame.type != ReplicationFrameType::kHeartbeat || frame.payload.size() != 8U)
    return Status{StatusCode::kInvalidArgument, "not a heartbeat frame"};
  std::size_t position = 0;
  std::uint64_t offset = 0;
  if (!Get64(frame.payload, position, offset))
    return Status{StatusCode::kCorruption, "heartbeat frame"};
  return offset;
}

Result<std::string> ReplicationFrameCodec::EncodeFullSyncRequest(std::uint64_t next_offset) {
  if (next_offset == 0) return Status{StatusCode::kInvalidArgument, "invalid full-sync offset"};
  std::string payload;
  Put64(payload, next_offset);
  return Wrap(ReplicationFrameType::kFullSyncRequest, std::move(payload));
}

Result<std::uint64_t> ReplicationFrameCodec::DecodeFullSyncRequest(const ReplicationFrame& frame) {
  if (frame.type != ReplicationFrameType::kFullSyncRequest || frame.payload.size() != 8U)
    return Status{StatusCode::kInvalidArgument, "not a full-sync request frame"};
  std::size_t position = 0;
  std::uint64_t next_offset = 0;
  if (!Get64(frame.payload, position, next_offset) || next_offset == 0)
    return Status{StatusCode::kCorruption, "full-sync request frame"};
  return next_offset;
}

Result<std::string> ReplicationFrameCodec::ReassembleChunks(
    const std::vector<ReplicationFrame>& chunks) {
  if (chunks.empty() || chunks.front().type != ReplicationFrameType::kChunk)
    return Status{StatusCode::kInvalidArgument, "invalid replication chunks"};
  const auto id = chunks.front().transfer_id;
  const auto count = chunks.front().chunk_count;
  const auto total_size = chunks.front().chunk_total_size;
  if (count == 0 || count > 16'384U || total_size > kMaxTransferBytes)
    return Status{StatusCode::kLimitExceeded, "replication chunk bounds"};
  std::vector<const ReplicationFrame*> ordered(count, nullptr);
  std::size_t accumulated = 0;
  for (const auto& chunk : chunks) {
    if (chunk.type != ReplicationFrameType::kChunk || chunk.transfer_id != id ||
        chunk.chunk_type != chunks.front().chunk_type || chunk.chunk_count != count ||
        chunk.chunk_index >= count || ordered[chunk.chunk_index] != nullptr)
      return Status{StatusCode::kCorruption, "replication chunk sequence"};
    ordered[chunk.chunk_index] = &chunk;
    if (chunk.payload.size() > total_size - std::min(total_size, accumulated))
      return Status{StatusCode::kLimitExceeded, "replication chunk payload exceeds total"};
    accumulated += chunk.payload.size();
  }
  std::string output;
  for (const auto* chunk : ordered) {
    if (chunk == nullptr) return Status{StatusCode::kBusy, "replication chunks incomplete"};
    output.append(chunk->payload);
  }
  if (output.size() != total_size)
    return Status{StatusCode::kCorruption, "replication chunk total size mismatch"};
  return output;
}

}  // namespace kvstore
