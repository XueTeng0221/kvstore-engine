#include <array>
#include <iostream>

#include "kvstore/common/crc32.hpp"
#include "kvstore/integration/protocol.hpp"

int main() {
  using namespace kvstore;
  using namespace kvstore::integration;
  kvcache::ChunkRegistry registry;
  kvcache::MatchIndex index;
  Session server(registry, index, "mock-tenant", "mock-model");
  v1::Request request;
  request.mutable_trace()->set_request_id("mock-request");
  request.mutable_trace()->set_tenant_id("mock-tenant");
  request.mutable_trace()->set_model_id("mock-model");
  auto call = [&]() -> Result<v1::Response> {
    auto bytes = Codec::EncodeRequest(request);
    if (!bytes.ok()) return bytes.status();
    auto response = server.Exchange(bytes.value());
    if (!response.ok()) return response.status();
    return Codec::DecodeResponse(response.value());
  };
  request.set_operation(v1::NEGOTIATE);
  request.mutable_capabilities()->set_major(kMajor);
  request.mutable_capabilities()->set_pinned_cpu(true);
  auto response = call();
  if (!response.ok() || response.value().status() != v1::OK) return 1;
  kvcache::TensorManifest m;
  m.tenant_id = "mock-tenant";
  m.model_id = "mock-model";
  m.model_revision = "revision";
  m.tokenizer_revision = "tokenizer";
  m.cache_format = "mock";
  m.token_count = 4;
  m.layer_count = 1;
  m.shape = {2, 1, 4, 2, 4};
  m.axis_order = {kvcache::TensorAxis::kKeyValue, kvcache::TensorAxis::kLayer,
                  kvcache::TensorAxis::kToken, kvcache::TensorAxis::kHead,
                  kvcache::TensorAxis::kHeadDimension};
  m.strides_bytes = {64, 64, 16, 8, 2};
  m.payload_bytes = 128;
  m.chunk_bytes = 128;
  m.chunk_count = 1;
  const std::array<std::uint32_t, 4> tokens{1, 2, 3, 4};
  const Bytes payload(128, std::byte{0x42});
  m.token_digest = kvcache::TokenDigest(tokens).value();
  m.payload_digest = kvcache::Sha256(payload).value();
  request.set_operation(v1::RESERVE);
  *request.mutable_manifest() = ToProto(m);
  response = call();
  if (!response.ok() || response.value().status() != v1::OK) return 2;
  request.set_reservation_id(response.value().reservation_id());
  request.set_operation(v1::PUT);
  request.set_payload(payload.data(), payload.size());
  request.set_payload_checksum(Crc32(payload));
  response = call();
  if (!response.ok() || response.value().status() != v1::OK) return 3;
  request.clear_payload();
  request.set_operation(v1::COMMIT);
  response = call();
  if (!response.ok() || response.value().status() != v1::OK) return 4;
  request.set_lease_id(response.value().lease_id());
  request.set_operation(v1::RELEASE);
  response = call();
  if (!response.ok() || response.value().status() != v1::OK) return 5;
  request.set_operation(v1::LOOKUP);
  request.mutable_manifest()->clear_payload_digest();
  request.set_exact(true);
  for (auto token : tokens) request.add_token_ids(token);
  response = call();
  if (!response.ok() || response.value().status() != v1::OK || response.value().hit_tokens() != 4)
    return 6;
  request.set_lease_id(response.value().lease_id());
  request.set_operation(v1::GET);
  response = call();
  if (!response.ok() || response.value().status() != v1::OK ||
      response.value().payload() != std::string(128, 'B') ||
      response.value().payload_checksum() != Crc32(payload))
    return 7;
  request.set_operation(v1::RELEASE);
  response = call();
  if (!response.ok() || response.value().status() != v1::OK || registry.Size() != 1) return 8;
  std::cout << "Protobuf mock: negotiate/reserve/put/commit/release/lookup/get/release passed\n";
}
