#include "kvstore/integration/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <new>

namespace kvstore::integration {
namespace {
std::atomic<std::uint64_t> next_lease{1};
constexpr std::size_t kMaxHandles = 64;
void Put32(Bytes& b, std::uint32_t v) {
  for (int i = 3; i >= 0; --i) b.push_back(std::byte((v >> (i * 8)) & 255));
}
std::uint32_t Get32(const std::byte* p) {
  std::uint32_t n = 0;
  for (int i = 0; i < 4; ++i) n = (n << 8) | std::to_integer<std::uint32_t>(p[i]);
  return n;
}
v1::StatusCode WireStatus(StatusCode c) {
  switch (c) {
    case StatusCode::kOk:
      return v1::OK;
    case StatusCode::kInvalidArgument:
      return v1::INVALID_ARGUMENT;
    case StatusCode::kNotFound:
      return v1::NOT_FOUND;
    case StatusCode::kAlreadyExists:
      return v1::ALREADY_EXISTS;
    case StatusCode::kLimitExceeded:
      return v1::LIMIT_EXCEEDED;
    case StatusCode::kCorruption:
      return v1::CORRUPTION;
    case StatusCode::kUnsupported:
      return v1::UNSUPPORTED;
    case StatusCode::kCancelled:
      return v1::CANCELLED;
    case StatusCode::kDeadlineExceeded:
      return v1::DEADLINE_EXCEEDED;
    case StatusCode::kBusy:
      return v1::BUSY;
    case StatusCode::kIoError:
      return v1::IO_ERROR;
    default:
      return v1::INTERNAL;
  }
}
template <typename T>
Result<Bytes> EncodeMessage(const T& message, bool response) {
  if (!v1::Operation_IsValid(message.operation()) || message.operation() == v1::UNSPECIFIED)
    return Status{StatusCode::kInvalidArgument, "operation"};
  const auto size = message.ByteSizeLong();
  if (size > Codec::kMaxFrame - Codec::kHeader)
    return Status{StatusCode::kLimitExceeded, "protobuf size"};
  Frame f{static_cast<std::uint8_t>(static_cast<unsigned>(message.operation()) |
                                    (response ? 128U : 0U)),
          kMajor, kMinor, Bytes(size)};
  if (!message.SerializeToArray(f.payload.data(), static_cast<int>(size)))
    return Status{StatusCode::kCorruption, "protobuf serialization"};
  return Codec::Encode(f);
}
template <typename T>
Result<T> DecodeMessage(ByteView bytes, bool response) {
  auto frame = Codec::Decode(bytes);
  if (!frame.ok()) return frame.status();
  T message;
  if (!message.ParseFromArray(frame.value().payload.data(),
                              static_cast<int>(frame.value().payload.size())) ||
      !v1::Operation_IsValid(message.operation()) || message.operation() == v1::UNSPECIFIED ||
      frame.value().operation !=
          (static_cast<unsigned>(message.operation()) | (response ? 128U : 0U)))
    return Status{StatusCode::kCorruption, "protobuf operation or message"};
  return message;
}
}  // namespace
Result<Bytes> Codec::Encode(const Frame& f) try {
  if (f.major != kMajor || f.minor > kMinor || f.payload.size() > kMaxFrame - kHeader)
    return Status(StatusCode::kInvalidArgument, "frame");
  Bytes b;
  b.reserve(kHeader + f.payload.size());
  b.insert(b.end(), {std::byte{'K'}, std::byte{'V'}, std::byte{'P'}, std::byte{1},
                     std::byte{f.operation}, std::byte(f.major >> 8), std::byte(f.major),
                     std::byte(f.minor >> 8), std::byte(f.minor)});
  Put32(b, static_cast<std::uint32_t>(f.payload.size()));
  b.insert(b.end(), f.payload.begin(), f.payload.end());
  return b;
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "frame allocation"};
}
Result<Frame> Codec::Decode(ByteView b) try {
  if (b.size() < kHeader || b.size() > kMaxFrame || b[0] != std::byte{'K'} ||
      b[1] != std::byte{'V'} || b[2] != std::byte{'P'} || b[3] != std::byte{1})
    return Status(StatusCode::kCorruption, "frame header");
  const auto n = Get32(b.data() + 9);
  if (n > kMaxFrame - kHeader || n != b.size() - kHeader)
    return Status(StatusCode::kCorruption, "frame length");
  const auto major =
      std::uint16_t(std::to_integer<unsigned>(b[5]) << 8 | std::to_integer<unsigned>(b[6]));
  const auto minor =
      std::uint16_t(std::to_integer<unsigned>(b[7]) << 8 | std::to_integer<unsigned>(b[8]));
  if (major != kMajor || minor > kMinor) return Status(StatusCode::kUnsupported, "version");
  return Frame{std::to_integer<std::uint8_t>(b[4]), major, minor, {b.begin() + kHeader, b.end()}};
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "frame allocation"};
}
Result<std::vector<Frame>> Codec::Feed(Bytes& buf, ByteView in) try {
  if (buf.size() > kMaxFrame || in.size() > kMaxFrame - buf.size())
    return Status{StatusCode::kLimitExceeded, "input budget"};
  buf.insert(buf.end(), in.begin(), in.end());
  std::vector<Frame> out;
  std::size_t offset = 0;
  while (buf.size() - offset >= kHeader) {
    const auto n = Get32(buf.data() + offset + 9);
    if (n > kMaxFrame - kHeader) return Status{StatusCode::kLimitExceeded, "frame"};
    if (buf.size() - offset < kHeader + n) break;
    auto f = Decode(ByteView(buf).subspan(offset, kHeader + n));
    if (!f.ok()) return f.status();
    // Bound output object overhead as well as payload bytes.
    if (out.size() == 4096) return Status{StatusCode::kLimitExceeded, "frame count"};
    out.push_back(std::move(f.value()));
    offset += kHeader + n;
  }
  buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(offset));
  return out;
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "input allocation"};
}
Result<Bytes> Codec::EncodeRequest(const v1::Request& r) try {
  return EncodeMessage(r, false);
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "protobuf allocation"};
}
Result<v1::Request> Codec::DecodeRequest(ByteView b) try {
  return DecodeMessage<v1::Request>(b, false);
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "protobuf allocation"};
}
Result<Bytes> Codec::EncodeResponse(const v1::Response& r) try {
  return EncodeMessage(r, true);
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "protobuf allocation"};
}
Result<v1::Response> Codec::DecodeResponse(ByteView b) try {
  return DecodeMessage<v1::Response>(b, true);
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "protobuf allocation"};
}

v1::TensorManifest ToProto(const kvcache::TensorManifest& m) {
  v1::TensorManifest p;
#define FIELD(name) p.set_##name(m.name)
  FIELD(version);
  FIELD(tenant_id);
  FIELD(model_id);
  FIELD(model_revision);
  FIELD(adapter_id);
  FIELD(adapter_revision);
  FIELD(tokenizer_revision);
  FIELD(cache_format);
  FIELD(cache_format_version);
  FIELD(token_count);
  FIELD(layer_begin);
  FIELD(layer_count);
  FIELD(block_tokens);
  FIELD(tensor_parallel_rank);
  FIELD(tensor_parallel_size);
  FIELD(pipeline_parallel_rank);
  FIELD(pipeline_parallel_size);
  FIELD(payload_bytes);
  FIELD(chunk_bytes);
  FIELD(chunk_alignment_bytes);
  FIELD(chunk_count);
  FIELD(created_at_ns);
  FIELD(accessed_at_ns);
#undef FIELD
  p.set_token_digest(m.token_digest.data(), m.token_digest.size());
  p.set_payload_digest(m.payload_digest.data(), m.payload_digest.size());
  p.set_dtype(static_cast<unsigned>(m.dtype));
  p.set_layout(static_cast<unsigned>(m.layout));
  p.set_key_value_packing(static_cast<unsigned>(m.key_value_packing));
  p.set_compression(static_cast<unsigned>(m.compression));
  p.set_device_kind(static_cast<unsigned>(m.device.kind));
  p.set_device_index(m.device.index);
  for (auto x : m.shape) p.add_shape(x);
  for (auto x : m.strides_bytes) p.add_strides_bytes(x);
  for (auto x : m.axis_order) p.add_axis_order(static_cast<unsigned>(x));
  return p;
}
Result<kvcache::TensorManifest> FromProto(const v1::TensorManifest& p, ManifestUse use) try {
  if (p.version() != kvcache::kManifestVersion || p.token_digest().size() != 32 ||
      (p.payload_digest().size() != 32 &&
       !(use == ManifestUse::kQuery && p.payload_digest().empty())) ||
      p.dtype() < 1 || p.dtype() > 3 || p.layout() < 1 || p.layout() > 2 ||
      p.key_value_packing() < 1 || p.key_value_packing() > 2 || p.device_kind() < 1 ||
      p.device_kind() > 2 || p.compression() != 0 || p.shape_size() > 6 ||
      p.axis_order_size() > 6 || p.strides_bytes_size() > 6)
    return Status{StatusCode::kInvalidArgument, "manifest wire fields"};
  kvcache::TensorManifest m;
  m.version = static_cast<std::uint16_t>(p.version());
#define FIELD(name) m.name = p.name()
  FIELD(tenant_id);
  FIELD(model_id);
  FIELD(model_revision);
  FIELD(adapter_id);
  FIELD(adapter_revision);
  FIELD(tokenizer_revision);
  FIELD(cache_format);
  FIELD(cache_format_version);
  FIELD(token_count);
  FIELD(layer_begin);
  FIELD(layer_count);
  FIELD(block_tokens);
  FIELD(tensor_parallel_rank);
  FIELD(tensor_parallel_size);
  FIELD(pipeline_parallel_rank);
  FIELD(pipeline_parallel_size);
  FIELD(payload_bytes);
  FIELD(chunk_bytes);
  FIELD(chunk_alignment_bytes);
  FIELD(chunk_count);
  FIELD(created_at_ns);
  FIELD(accessed_at_ns);
#undef FIELD
  std::memcpy(m.token_digest.data(), p.token_digest().data(), 32);
  if (!p.payload_digest().empty())
    std::memcpy(m.payload_digest.data(), p.payload_digest().data(), 32);
  m.dtype = static_cast<kvcache::DType>(p.dtype());
  m.layout = static_cast<kvcache::TensorLayout>(p.layout());
  m.key_value_packing = static_cast<kvcache::KeyValuePacking>(p.key_value_packing());
  m.device = {static_cast<kvcache::DeviceKind>(p.device_kind()), p.device_index()};
  m.shape.assign(p.shape().begin(), p.shape().end());
  m.strides_bytes.assign(p.strides_bytes().begin(), p.strides_bytes().end());
  for (auto x : p.axis_order()) {
    if (x < 1 || x > 6) return Status{StatusCode::kInvalidArgument, "tensor axis"};
    m.axis_order.push_back(static_cast<kvcache::TensorAxis>(x));
  }
  const auto status = use == ManifestUse::kQuery ? kvcache::ValidateManifestIdentity(m)
                                                 : kvcache::ValidateManifest(m);
  if (!status.ok()) return status;
  return m;
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "manifest allocation"};
}

Session::Session(kvcache::ChunkRegistry& r, kvcache::MatchIndex& i, std::string t, std::string m)
    : registry_(r), index_(i), tenant_(std::move(t)), model_(std::move(m)) {
  reservations_.reserve(kMaxHandles);
  leases_.reserve(kMaxHandles);
}
Session::~Session() { Disconnect(); }
Status Session::PrepareLease(const kvcache::TensorManifest& m, v1::Response& out) {
  if (leases_.size() >= kMaxHandles) return {StatusCode::kLimitExceeded, "lease budget"};
  *out.mutable_manifest() = ToProto(m);
  auto id = next_lease.load();
  do {
    if (id == std::numeric_limits<std::uint64_t>::max())
      return {StatusCode::kLimitExceeded, "lease IDs exhausted"};
  } while (!next_lease.compare_exchange_weak(id, id + 1));
  out.set_lease_id(id);
  return Status::Ok();
}
Status Session::Pin(kvcache::ResidentHandle h, v1::Response& out) {
  if (auto status = PrepareLease(h.manifest(), out); !status.ok()) return status;
  leases_.push_back({out.lease_id(), std::move(h)});
  return Status::Ok();
}
Status Session::Execute(const v1::Request& r, v1::Response& out) {
  if (closed_) return {StatusCode::kCancelled, "session closed"};
  const auto& t = r.trace();
  if (t.request_id().empty() || t.request_id().size() > 128 || tenant_.empty() || model_.empty() ||
      t.tenant_id() != tenant_ || t.model_id() != model_ || tenant_.size() > 128 ||
      model_.size() > 256)
    return {StatusCode::kInvalidArgument, "trace identity"};
  if (r.cancelled()) return {StatusCode::kCancelled, "request cancelled"};
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  if (r.deadline_unix_ms() != 0 &&
      (now < 0 || r.deadline_unix_ms() <= static_cast<std::uint64_t>(now)))
    return {StatusCode::kDeadlineExceeded, "request deadline"};
  if (r.operation() == v1::NEGOTIATE) {
    if (negotiated_) return {StatusCode::kAlreadyExists, "already negotiated"};
    const auto& c = r.capabilities();
    if (c.major() != kMajor || c.minor() > kMinor || !c.pinned_cpu())
      return {StatusCode::kUnsupported, "capabilities"};
    auto* accepted = out.mutable_capabilities();
    accepted->set_major(kMajor);
    accepted->set_minor(kMinor);
    accepted->set_pinned_cpu(true);
    accepted->set_cuda_ipc(false);
    negotiated_ = true;
    return Status::Ok();
  }
  if (!negotiated_) return {StatusCode::kUnsupported, "negotiation required"};
  if (r.operation() == v1::RESERVE || r.operation() == v1::LOOKUP) {
    if (r.manifest().tenant_id() != tenant_ || r.manifest().model_id() != model_)
      return {StatusCode::kInvalidArgument, "manifest identity"};
    auto m = FromProto(r.manifest(), r.operation() == v1::LOOKUP ? ManifestUse::kQuery
                                                                 : ManifestUse::kPublication);
    if (!m.ok()) return m.status();
    if (r.operation() == v1::RESERVE) {
      if (reservations_.size() == kMaxHandles)
        return {StatusCode::kLimitExceeded, "reservation budget"};
      if (m.value().chunk_bytes > Codec::kMaxFrame - 4096)
        return {StatusCode::kLimitExceeded, "chunk wire budget"};
      // Copy metadata before acquiring the registry reservation. Slot storage is preallocated.
      Reservation tracking{0, std::move(m.value())};
      auto id = registry_.Reserve(tracking.manifest);
      if (!id.ok()) return id.status();
      if (fail_tracking_) {
        fail_tracking_ = false;
        const auto aborted = registry_.Abort(id.value());
        if (!aborted.ok()) return aborted;
        return {StatusCode::kLimitExceeded, "injected tracking allocation failure"};
      }
      tracking.id = id.value();
      reservations_.push_back(std::move(tracking));
      out.set_reservation_id(id.value());
      return Status::Ok();
    }
    if (leases_.size() == kMaxHandles) return {StatusCode::kLimitExceeded, "lease budget"};
    const std::span<const std::uint32_t> tokens(r.token_ids().data(),
                                                static_cast<std::size_t>(r.token_ids_size()));
    const std::span<const std::uint64_t> prefixes(
        r.prefix_lengths().data(), static_cast<std::size_t>(r.prefix_lengths_size()));
    auto match = r.exact() ? index_.Exact(m.value(), tokens)
                           : index_.LongestPrefix(m.value(), tokens, prefixes);
    if (!match.ok()) return match.status();
    auto h = registry_.Lookup(match.value().source_manifest);
    if (!h.ok()) return h.status();
    const auto& hit = match.value();
    out.set_hit_tokens(hit.hit_tokens);
    auto* range = out.add_hit_ranges();
    range->set_begin(0);
    range->set_end(hit.hit_tokens);
    for (auto x : hit.missing_tokens) {
      auto* missing = out.add_recompute_ranges();
      missing->set_begin(x.begin);
      missing->set_end(x.end);
    }
    for (auto x : hit.chunk_indices) out.add_chunk_indices(x);
    out.set_layer_begin(hit.layer_begin);
    out.set_layer_count(hit.layer_count);
    out.set_recompute(!hit.missing_tokens.empty());
    return Pin(std::move(h.value()), out);
  }
  if (r.operation() == v1::GET || r.operation() == v1::RELEASE) {
    auto it = std::ranges::find(leases_, r.lease_id(), &Lease::id);
    if (it == leases_.end()) return {StatusCode::kNotFound, "session lease"};
    if (r.operation() == v1::RELEASE) {
      leases_.erase(it);
      return Status::Ok();
    }
    auto bytes = it->handle.Chunk(r.chunk_index());
    if (!bytes.ok()) return bytes.status();
    // Reserve generous response metadata overhead before copying a chunk.
    if (bytes.value().size() > Codec::kMaxFrame - 4096)
      return {StatusCode::kLimitExceeded, "chunk response budget"};
    out.set_payload(bytes.value().data(), bytes.value().size());
    auto crc = it->handle.ChunkChecksum(r.chunk_index());
    if (!crc.ok()) return crc.status();
    out.set_payload_checksum(crc.value());
    return Status::Ok();
  }
  if (r.operation() != v1::PUT && r.operation() != v1::COMMIT && r.operation() != v1::ABORT &&
      r.operation() != v1::CANCEL)
    return {StatusCode::kUnsupported, "operation"};
  auto it = std::ranges::find(reservations_, r.reservation_id(), &Reservation::id);
  if (it == reservations_.end()) return {StatusCode::kNotFound, "session reservation"};
  if (r.operation() == v1::PUT)
    return registry_.Put(
        it->id, r.chunk_index(),
        {reinterpret_cast<const std::byte*>(r.payload().data()), r.payload().size()},
        r.payload_checksum());
  if (r.operation() == v1::ABORT || r.operation() == v1::CANCEL) {
    auto status = registry_.Abort(it->id);
    if (status.ok()) reservations_.erase(it);
    return status;
  }
  // No lease allocation after committing. Index insertion is staged first; a lookup
  // racing this stage can only miss because the registry has not published yet.
  if (leases_.size() == kMaxHandles) return {StatusCode::kLimitExceeded, "lease budget"};
  auto key = kvcache::CanonicalCacheKey(it->manifest);
  if (!key.ok()) return key.status();
  if (auto status = PrepareLease(it->manifest, out); !status.ok()) return status;
  auto inserted = index_.Insert(it->manifest, key.value());
  if (!inserted.ok()) return inserted;
  auto h = registry_.Commit(it->id);
  if (!h.ok()) {
    index_.RollbackInsert(key.value());
    return h.status();
  }
  reservations_.erase(it);
  leases_.push_back({out.lease_id(), std::move(h.value())});
  return Status::Ok();
}
v1::Response Session::Dispatch(const v1::Request& r) {
  std::lock_guard lock(mutex_);
  v1::Response out;
  out.set_operation(r.operation());
  Status status;
  try {
    // Bound direct mock callers too, not only the serialized ingress.
    if (r.ByteSizeLong() > Codec::kMaxFrame - Codec::kHeader)
      status = {StatusCode::kLimitExceeded, "request budget"};
    else {
      *out.mutable_trace() = r.trace();
      status = Execute(r, out);
    }
  } catch (const std::bad_alloc&) {
    status = {StatusCode::kLimitExceeded, "session allocation"};
  } catch (...) {
    status = {StatusCode::kInternal, "session failure"};
  }
  if (!status.ok()) {
    out.clear_manifest();
    out.clear_payload();
    out.clear_hit_ranges();
    out.clear_chunk_indices();
    out.clear_recompute_ranges();
    out.set_hit_tokens(0);
    out.set_lease_id(0);
    out.set_recompute(true);
    if (r.operation() == v1::LOOKUP && r.manifest().token_count() != 0) {
      auto* range = out.add_recompute_ranges();
      range->set_begin(0);
      range->set_end(r.manifest().token_count());
    }
  }
  out.set_status(WireStatus(status.code()));
  out.set_message(status.message().data(), status.message().size());
  return out;
}
Result<Bytes> Session::Exchange(ByteView bytes) try {
  auto request = Codec::DecodeRequest(bytes);
  if (!request.ok()) return request.status();
  auto result = Codec::EncodeResponse(Dispatch(request.value()));
  // A lost response must not strand a lease or pending upload on a healthy session.
  if (!result.ok()) Disconnect();
  return result;
} catch (const std::bad_alloc&) {
  Disconnect();
  return Status{StatusCode::kLimitExceeded, "exchange allocation"};
}
void Session::Disconnect() noexcept {
  std::lock_guard lock(mutex_);
  for (const auto& r : reservations_) {
    static_cast<void>(registry_.Abort(r.id));
  }
  reservations_.clear();
  leases_.clear();
  negotiated_ = false;
  closed_ = true;
}
}  // namespace kvstore::integration
