#include "kvstore/integration/sglang/hicache_storage.hpp"
#include "kvstore/common/crc32.hpp"

#include <limits>

namespace kvstore::integration::sglang {
namespace {
v1::TraceContext Trace(std::string_view tenant, std::string_view model, std::uint64_t n) {
  v1::TraceContext t;
  t.set_request_id("sglang-" + std::to_string(n));
  t.set_tenant_id(tenant.data(), tenant.size());
  t.set_model_id(model.data(), model.size());
  return t;
}
}

Result<kvcache::TensorManifest> CanonicalManifest(const PrefixQuery& q, std::string tenant,
                                                   std::string model) {
  if (q.token_ids.empty() || q.token_ids.size() > std::numeric_limits<std::uint32_t>::max())
    return Status{StatusCode::kInvalidArgument, "empty or oversized radix prefix"};
  kvcache::TensorManifest m;
  m.tenant_id = std::move(tenant); m.model_id = std::move(model);
  m.model_revision = q.metadata.model_revision; m.tokenizer_revision = q.metadata.tokenizer_revision;
  m.cache_format = "sglang-hicache-interface_v1"; m.cache_format_version = 1;
  m.token_count = q.token_ids.size(); m.layer_begin = q.metadata.layer_begin;
  m.layer_count = q.metadata.layer_count; m.dtype = q.metadata.dtype; m.block_tokens = q.metadata.block_tokens;
  m.device = q.metadata.device; m.layout = kvcache::TensorLayout::kBlockMajor;
  m.key_value_packing = kvcache::KeyValuePacking::kInterleaved;
  m.shape = {2, m.layer_count, m.token_count, q.metadata.head_count, q.metadata.head_dimension};
  m.axis_order = {kvcache::TensorAxis::kKeyValue, kvcache::TensorAxis::kLayer,
                  kvcache::TensorAxis::kToken, kvcache::TensorAxis::kHead,
                  kvcache::TensorAxis::kHeadDimension};
  m.token_digest = kvcache::TokenDigest(q.token_ids).value();
  return m;
}

HiCacheStorage::HiCacheStorage(Session& s, std::string tenant, std::string model)
    : session_(s), tenant_(std::move(tenant)), model_(std::move(model)) {}
Result<v1::Response> HiCacheStorage::Call(v1::Request& r) {
  *r.mutable_trace() = Trace(tenant_, model_, sequence_++);
  auto bytes = Codec::EncodeRequest(r); if (!bytes.ok()) return bytes.status();
  auto reply = session_.Exchange(bytes.value()); if (!reply.ok()) return reply.status();
  return Codec::DecodeResponse(reply.value());
}
Result<v1::Response> HiCacheStorage::Lookup(const PrefixQuery& q, std::uint64_t deadline, bool cancelled) {
  auto m = CanonicalManifest(q, tenant_, model_); if (!m.ok()) return m.status();
  v1::Request r; r.set_operation(v1::LOOKUP); *r.mutable_manifest() = ToProto(m.value());
  r.set_exact(q.exact); r.set_deadline_unix_ms(deadline); r.set_cancelled(cancelled);
  for (auto x : q.token_ids) r.add_token_ids(x);
  for (auto x : q.prefix_lengths) r.add_prefix_lengths(x);
  return Call(r);
}
Result<v1::Response> HiCacheStorage::Publish(const kvcache::TensorManifest& m, std::span<const Bytes> chunks) {
  v1::Request r; r.set_operation(v1::RESERVE); *r.mutable_manifest() = ToProto(m);
  auto reserved = Call(r); if (!reserved.ok() || reserved.value().status() != v1::OK) return reserved;
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    r.Clear(); r.set_operation(v1::PUT); r.set_reservation_id(reserved.value().reservation_id());
    r.set_chunk_index(static_cast<std::uint32_t>(i)); r.set_payload(chunks[i].data(), chunks[i].size());
    r.set_payload_checksum(kvstore::Crc32(chunks[i]));
    auto put = Call(r);
    if (!put.ok() || put.value().status() != v1::OK) {
      const auto ignored = Abort(reserved.value().reservation_id()); (void)ignored;
      return put;
    }
  }
  r.Clear(); r.set_operation(v1::COMMIT); r.set_reservation_id(reserved.value().reservation_id()); return Call(r);
}
Status HiCacheStorage::Release(std::uint64_t id) { v1::Request r; r.set_operation(v1::RELEASE); r.set_lease_id(id); auto x=Call(r); return x.ok() && x.value().status()==v1::OK ? Status::Ok() : Status{StatusCode::kNotFound,"release failed"}; }
Status HiCacheStorage::Abort(std::uint64_t id) { v1::Request r; r.set_operation(v1::ABORT); r.set_reservation_id(id); auto x=Call(r); return x.ok() && x.value().status()==v1::OK ? Status::Ok() : Status{StatusCode::kCancelled,"abort failed"}; }
}  // namespace kvstore::integration::sglang
