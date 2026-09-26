#include "kvstore/integration/vllm/adapter.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <new>

#include "kvstore/common/crc32.hpp"

namespace kvstore::integration::vllm {
namespace {
Status Check(const Request& request) {
  if (request.stop.stop_requested()) return {StatusCode::kCancelled, "request cancelled"};
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  if (request.deadline_unix_ms != 0 && now >= 0 &&
      request.deadline_unix_ms <= static_cast<std::uint64_t>(now))
    return {StatusCode::kDeadlineExceeded, "request deadline expired"};
  return Status::Ok();
}
Status RemoteStatus(const v1::Response& response) {
  switch (response.status()) {
    case v1::OK:
      return Status::Ok();
    case v1::INVALID_ARGUMENT:
      return {StatusCode::kInvalidArgument, response.message()};
    case v1::NOT_FOUND:
      return {StatusCode::kNotFound, response.message()};
    case v1::ALREADY_EXISTS:
      return {StatusCode::kAlreadyExists, response.message()};
    case v1::LIMIT_EXCEEDED:
      return {StatusCode::kLimitExceeded, response.message()};
    case v1::CORRUPTION:
      return {StatusCode::kCorruption, response.message()};
    case v1::UNSUPPORTED:
      return {StatusCode::kUnsupported, response.message()};
    case v1::CANCELLED:
      return {StatusCode::kCancelled, response.message()};
    case v1::DEADLINE_EXCEEDED:
      return {StatusCode::kDeadlineExceeded, response.message()};
    case v1::BUSY:
      return {StatusCode::kBusy, response.message()};
    case v1::IO_ERROR:
      return {StatusCode::kIoError, response.message()};
    default:
      return {StatusCode::kInternal, "invalid or internal sidecar response"};
  }
}
bool Revision(const std::string& value) {
  return value.size() == 40 && std::ranges::all_of(value, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
bool Active(PublicationState state) {
  return state == PublicationState::kAwaitingCopy || state == PublicationState::kReserve ||
         state == PublicationState::kPut || state == PublicationState::kCommit;
}
}  // namespace

Adapter::Adapter(std::unique_ptr<Transport> transport, Options options)
    : transport_(std::move(transport)), options_(std::move(options)) {}
Adapter::~Adapter() { Close(); }
void Adapter::Close() noexcept {
  if (transport_) transport_->Disconnect();
  connected_ = false;
  closed_ = true;
  reservation_ = 0;
  Bytes{}.swap(payload_);
  if (Active(state_)) state_ = PublicationState::kFailed;
}
v1::Request Adapter::Message(v1::Operation op, const Request& request) const {
  v1::Request result;
  result.set_operation(op);
  result.mutable_trace()->set_request_id(request.id);
  result.mutable_trace()->set_model_id(std::string(kModel));
  result.mutable_trace()->set_tenant_id(options_.tenant);
  result.set_deadline_unix_ms(request.deadline_unix_ms);
  result.set_cancelled(request.stop.stop_requested());
  return result;
}
Result<v1::Response> Adapter::Call(const v1::Request& request) {
  if (closed_ || !transport_ || (!connected_ && request.operation() != v1::NEGOTIATE))
    return Status{StatusCode::kCancelled, "adapter transport not connected"};
  auto frame = Codec::EncodeRequest(request);
  if (!frame.ok()) return frame.status();
  auto reply = transport_->Exchange(frame.value());
  if (!reply.ok()) {
    Close();
    return reply.status();
  }
  auto decoded = Codec::DecodeResponse(reply.value());
  if (!decoded.ok()) {
    Close();
    return decoded.status();
  }
  if (decoded.value().operation() != request.operation() ||
      decoded.value().trace().SerializeAsString() != request.trace().SerializeAsString()) {
    Close();
    return Status{StatusCode::kCorruption, "sidecar response correlation mismatch"};
  }
  return decoded;
}
Status Adapter::Connect() try {
  if (options_.framework_version != kVersion || !Revision(options_.model_revision) ||
      !Revision(options_.tokenizer_revision) || options_.tenant.empty() ||
      options_.tenant.size() > 256)
    return {StatusCode::kUnsupported, "requires vLLM 0.29.0, tenant and pinned 40-hex revisions"};
  if (connected_) return {StatusCode::kAlreadyExists, "adapter already connected"};
  Request control;
  control.id = "negotiate";
  auto request = Message(v1::NEGOTIATE, control);
  request.mutable_capabilities()->set_major(kMajor);
  request.mutable_capabilities()->set_minor(kMinor);
  request.mutable_capabilities()->set_pinned_cpu(true);
  auto reply = Call(request);
  if (!reply.ok()) return reply.status();
  auto status = RemoteStatus(reply.value());
  if (!status.ok()) return status;
  const auto& caps = reply.value().capabilities();
  if (caps.major() != kMajor || caps.minor() != kMinor || !caps.pinned_cpu() || caps.cuda_ipc()) {
    Close();
    return {StatusCode::kUnsupported, "unsupported sidecar capabilities"};
  }
  connected_ = true;
  return Status::Ok();
} catch (const std::bad_alloc&) {
  Close();
  return {StatusCode::kLimitExceeded, "adapter allocation failed"};
}

Result<kvcache::TensorManifest> Adapter::Manifest(const Request& request) const try {
  if (options_.framework_version != kVersion || !Revision(options_.model_revision) ||
      !Revision(options_.tokenizer_revision))
    return Status{StatusCode::kUnsupported, "unsupported vLLM baseline"};
  if (request.id.empty() || request.id.size() > 256 || request.tokens.empty() ||
      request.tokens.size() > kMaxTokens ||
      std::ranges::any_of(request.tokens, [](auto token) { return token >= 151936; }))
    return Status{StatusCode::kInvalidArgument, "invalid request identity or Qwen token sequence"};
  kvcache::TensorManifest m;
  m.tenant_id = options_.tenant;
  m.model_id = kModel;
  m.model_revision = options_.model_revision;
  m.tokenizer_revision = options_.tokenizer_revision;
  m.cache_format = kFormat;
  m.token_count = request.tokens.size();
  auto digest = kvcache::TokenDigest(request.tokens);
  if (!digest.ok()) return digest.status();
  m.token_digest = digest.value();
  m.layer_count = 24;
  m.dtype = kvcache::DType::kBFloat16;
  m.layout = kvcache::TensorLayout::kBlockMajor;
  m.block_tokens = kBlockTokens;
  const auto blocks = (m.token_count + kBlockTokens - 1) / kBlockTokens;
  m.shape = {24, 2, blocks, kBlockTokens, 2, 64};
  using Axis = kvcache::TensorAxis;
  m.axis_order = {Axis::kLayer, Axis::kKeyValue, Axis::kBlock,
                  Axis::kToken, Axis::kHead,     Axis::kHeadDimension};
  m.strides_bytes = {8192 * blocks, 4096 * blocks, 4096, 256, 128, 2};
  m.payload_bytes = 196608 * blocks;
  m.chunk_bytes = 196608;
  m.chunk_count = static_cast<std::uint32_t>(blocks);
  const auto valid = kvcache::ValidateManifestIdentity(m);
  if (!valid.ok()) return valid;
  return m;
} catch (const std::bad_alloc&) {
  return Status{StatusCode::kLimitExceeded, "manifest allocation failed"};
}

Status Adapter::Cleanup(v1::Operation op, std::uint64_t id) {
  Request control;
  control.id = "cleanup";
  auto request = Message(op, control);  // Cleanup must not inherit an expired deadline/stop token.
  request.set_lease_id(id);
  request.set_reservation_id(id);
  auto reply = Call(request);
  auto status = reply.ok() ? RemoteStatus(reply.value()) : reply.status();
  if (!status.ok()) Close();
  return status;
}

PrefillResult Adapter::Prefill(const Request& request, bool exact, const BlockTable& blocks,
                               HitInjector& injector) try {
  PrefillResult result{Status::Ok(), 0, 0, request.tokens.size()};
  auto reject = [&](Status status) {
    result.status = std::move(status);
    return result;
  };
  auto status = Check(request);
  if (!status.ok()) return reject(status);
  auto query = Manifest(request);
  if (!query.ok()) return reject(query.status());
  if (blocks.locally_computed_tokens != 0 || blocks.ids.size() > kMaxTokens / kBlockTokens)
    return reject({StatusCode::kUnsupported, "local prefix composition or oversized block table"});
  auto message = Message(v1::LOOKUP, request);
  *message.mutable_manifest() = ToProto(query.value());
  for (auto token : request.tokens) message.add_token_ids(token);
  message.set_exact(exact);
  for (std::uint64_t n = kBlockTokens; n <= request.tokens.size(); n += kBlockTokens)
    message.add_prefix_lengths(n);
  auto reply = Call(message);
  if (!reply.ok()) return reject(reply.status());
  status = RemoteStatus(reply.value());
  if (!status.ok()) return reject(status);
  const auto lease = reply.value().lease_id();
  auto fail = [&](Status error) {
    if (lease != 0)
      static_cast<void>(Cleanup(v1::RELEASE, lease));  
      // Ignore cleanup failure, since the lease may have already expired.
    else
      Close();
    return reject(std::move(error));
  };
  const auto& response = reply.value();
  const auto count = response.hit_tokens();
  if (lease == 0 || count == 0 || count > request.tokens.size() || count % kBlockTokens != 0 ||
      (exact && count != request.tokens.size()))
    return fail({StatusCode::kCorruption, "invalid hit token count or lease"});
  auto source = FromProto(response.manifest());
  Request prefix = request;
  prefix.tokens.resize(static_cast<std::size_t>(count));
  auto expected = Manifest(prefix);
  if (!source.ok() || !expected.ok())
    return fail({StatusCode::kCorruption, "invalid hit manifest"});
  expected.value().payload_digest = source.value().payload_digest;
  expected.value().created_at_ns = source.value().created_at_ns;
  expected.value().accessed_at_ns = source.value().accessed_at_ns;
  if (source.value() != expected.value() || response.layer_begin() != 0 ||
      response.layer_count() != 24 || response.hit_ranges_size() != 1 ||
      response.hit_ranges(0).begin() != 0 || response.hit_ranges(0).end() != count ||
      response.recompute() != (count < request.tokens.size()) ||
      response.recompute_ranges_size() != (count < request.tokens.size() ? 1 : 0) ||
      (count < request.tokens.size() &&
       (response.recompute_ranges(0).begin() != count ||
        response.recompute_ranges(0).end() != request.tokens.size())) ||
      response.chunk_indices_size() != static_cast<int>(source.value().chunk_count))
    return fail({StatusCode::kCorruption, "incompatible hit metadata"});
  const auto needed = static_cast<std::size_t>(count / kBlockTokens);
  if (blocks.ids.size() < needed)
    return fail({StatusCode::kInvalidArgument, "insufficient destination blocks"});
  for (std::size_t i = 0; i < blocks.ids.size(); ++i) {
    if (blocks.ids[i] >= blocks.capacity ||
        std::find(blocks.ids.begin(), blocks.ids.begin() + static_cast<std::ptrdiff_t>(i),
                  blocks.ids[i]) != blocks.ids.begin() + static_cast<std::ptrdiff_t>(i))
      return fail({StatusCode::kInvalidArgument, "out-of-range or aliased destination block"});
  }
  Hit hit{request.id, std::move(source.value()), {}, {}};
  hit.destination_blocks.assign(blocks.ids.begin(),
                                blocks.ids.begin() + static_cast<std::ptrdiff_t>(needed));
  hit.payload.reserve(static_cast<std::size_t>(hit.manifest.payload_bytes));
  for (std::uint32_t i = 0; i < hit.manifest.chunk_count; ++i) {
    status = Check(request);
    if (!status.ok()) return fail(status);
    if (response.chunk_indices(static_cast<int>(i)) != i)
      return fail({StatusCode::kCorruption, "invalid source chunk order"});
    auto get = Message(v1::GET, request);
    get.set_lease_id(lease);
    get.set_chunk_index(i);
    auto chunk = Call(get);
    if (!chunk.ok()) return fail(chunk.status());
    status = RemoteStatus(chunk.value());
    if (!status.ok()) return fail(status);
    const auto& data = chunk.value().payload();
    const auto view = std::as_bytes(std::span(data.data(), data.size()));
    if (data.size() != hit.manifest.chunk_bytes || Crc32(view) != chunk.value().payload_checksum())
      return fail({StatusCode::kCorruption, "invalid source chunk bytes"});
    hit.payload.insert(hit.payload.end(), view.begin(), view.end());
  }
  auto digest = kvcache::Sha256(hit.payload);
  if (!digest.ok() || digest.value() != hit.manifest.payload_digest)
    return fail({StatusCode::kCorruption, "source payload digest mismatch"});
  static_cast<void>(Cleanup(v1::RELEASE, lease));
  if (!status.ok()) return reject(status);
  status = Check(request);
  if (!status.ok()) return reject(status);
  status = injector.Install(hit, request);
  if (!status.ok()) return reject(status);
  result.hit_tokens = count;
  result.recompute_begin = count;
  return result;
} catch (const std::bad_alloc&) {
  Close();
  return {{StatusCode::kLimitExceeded, "prefill allocation failed"}, 0, 0, request.tokens.size()};
}

Status Adapter::BeginPublication(const Request& request) try {
  if (!connected_ || closed_) return {StatusCode::kCancelled, "adapter not connected"};
  if (Active(state_)) return {StatusCode::kBusy, "one publication already in flight"};
  if (generation_ == std::numeric_limits<std::uint64_t>::max())
    return {StatusCode::kLimitExceeded, "publication generation exhausted"};
  auto status = Check(request);
  if (!status.ok()) return status;
  auto manifest = Manifest(request);
  if (!manifest.ok()) return manifest.status();
  if (request.tokens.size() % kBlockTokens != 0)
    return {StatusCode::kUnsupported, "only complete blocks may be published"};
  publication_ = request;
  manifest_ = std::move(manifest.value());
  next_chunk_ = 0;
  ++generation_;
  state_ = PublicationState::kAwaitingCopy;
  return Status::Ok();
} catch (const std::bad_alloc&) {
  Close();
  return {StatusCode::kLimitExceeded, "publication allocation failed"};
}
Status Adapter::HostCopyReady(std::uint64_t generation, ByteView bytes) try {
  if (generation != generation_) return {StatusCode::kCancelled, "stale host copy generation"};
  if (state_ != PublicationState::kAwaitingCopy)
    return {StatusCode::kInvalidArgument, "no pending host copy"};
  auto status = Check(publication_);
  if (!status.ok()) return FailPublication(status);
  if (bytes.size() != manifest_.payload_bytes)
    return FailPublication({StatusCode::kInvalidArgument, "host copy length mismatch"});
  auto digest = kvcache::Sha256(bytes);
  if (!digest.ok()) return FailPublication(digest.status());
  payload_.assign(bytes.begin(), bytes.end());
  manifest_.payload_digest = digest.value();
  state_ = PublicationState::kReserve;
  return Status::Ok();
} catch (const std::bad_alloc&) {
  Close();
  return {StatusCode::kLimitExceeded, "host staging allocation failed"};
}
Status Adapter::FailPublication(Status status) {
  if (reservation_ != 0) static_cast<void>(Cleanup(v1::CANCEL, reservation_));
  reservation_ = 0;
  Bytes{}.swap(payload_);
  state_ = status.code() == StatusCode::kCancelled ? PublicationState::kCancelled
                                                   : PublicationState::kFailed;
  return status;
}
Status Adapter::CancelPublication() try {
  if (!Active(state_)) return Status::Ok();
  auto status = reservation_ == 0 ? Status::Ok() : Cleanup(v1::CANCEL, reservation_);
  reservation_ = 0;
  Bytes{}.swap(payload_);
  state_ = PublicationState::kCancelled;
  return status;
} catch (const std::bad_alloc&) {
  Close();
  return {StatusCode::kLimitExceeded, "cancel allocation failed; transport closed"};
}
Status Adapter::AdvancePublication() try {
  if (!Active(state_)) return {StatusCode::kInvalidArgument, "no active publication"};
  auto status = Check(publication_);
  if (!status.ok()) return FailPublication(status);
  if (state_ == PublicationState::kAwaitingCopy)
    return {StatusCode::kBusy, "host copy not complete"};
  const auto op = state_ == PublicationState::kReserve ? v1::RESERVE
                  : state_ == PublicationState::kPut   ? v1::PUT
                                                       : v1::COMMIT;
  auto message = Message(op, publication_);
  message.set_reservation_id(reservation_);
  if (op == v1::RESERVE) *message.mutable_manifest() = ToProto(manifest_);
  if (op == v1::PUT) {
    const auto view = ByteView(payload_).subspan(
        static_cast<std::size_t>(next_chunk_) * manifest_.chunk_bytes, manifest_.chunk_bytes);
    message.set_chunk_index(next_chunk_);
    message.set_payload(view.data(), view.size());
    message.set_payload_checksum(Crc32(view));
  }
  auto reply = Call(message);
  if (!reply.ok()) return FailPublication(reply.status());
  status = RemoteStatus(reply.value());
  if (!status.ok()) return FailPublication(status);
  if (op == v1::RESERVE) {
    reservation_ = reply.value().reservation_id();
    if (reservation_ == 0) {
      Close();
      return FailPublication({StatusCode::kCorruption, "missing reservation ID"});
    }
    state_ = PublicationState::kPut;
  } else if (op == v1::PUT) {
    if (++next_chunk_ == manifest_.chunk_count) state_ = PublicationState::kCommit;
  } else {
    reservation_ = 0;
    const auto lease = reply.value().lease_id();
    if (lease == 0) {
      Close();
      return FailPublication({StatusCode::kCorruption, "missing commit lease"});
    }
    status = Cleanup(v1::RELEASE, lease);
    if (!status.ok()) return FailPublication(status);
    Bytes{}.swap(payload_);
    state_ = PublicationState::kPublished;
  }
  status = Check(publication_);
  if (!status.ok()) return FailPublication(status);
  return Status::Ok();
} catch (const std::bad_alloc&) {
  Close();
  return {StatusCode::kLimitExceeded, "publication allocation failed; transport closed"};
}
}  // namespace kvstore::integration::vllm
