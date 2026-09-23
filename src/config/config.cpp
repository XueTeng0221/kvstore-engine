#include "kvstore/config/config.hpp"

#include <arpa/inet.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string_view>
#include <type_traits>

namespace kvstore {
namespace {

using Json = nlohmann::json;

Status Invalid(std::string path, std::string message) {
  return {StatusCode::kInvalidArgument, std::move(path) + ": " + std::move(message)};
}

Status CheckKeys(const Json& value, std::string_view path,
                 std::initializer_list<std::string_view> allowed) {
  if (!value.is_object()) return Invalid(std::string(path), "expected object");
  const std::set<std::string_view> names(allowed);
  for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
    if (!names.contains(iterator.key())) {
      return Invalid(std::string(path) + "." + iterator.key(), "unknown field");
    }
  }
  return Status::Ok();
}

template <typename T>
Result<T> Required(const Json& object, std::string_view key, std::string_view path) {
  const std::string full_path = std::string(path) + "." + std::string(key);
  const auto iterator = object.find(key);
  if (iterator == object.end()) return Invalid(full_path, "required field is missing");
  try {
    if constexpr (std::is_unsigned_v<T> && !std::is_same_v<T, bool>) {
      if (!iterator->is_number_unsigned()) return Invalid(full_path, "expected unsigned integer");
      const auto value = iterator->get<std::uint64_t>();
      if (value > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
        return Invalid(full_path, "unsigned integer is out of range");
      }
      return static_cast<T>(value);
    }
    return iterator->get<T>();
  } catch (const Json::exception& error) {
    return Invalid(full_path, error.what());
  }
}

template <typename T>
Result<T> Get(const Json& root, std::string_view section, std::string_view key) {
  const auto section_iterator = root.find(section);
  if (section_iterator == root.end()) {
    return Invalid("$." + std::string(section), "required section is missing");
  }
  return Required<T>(*section_iterator, key, "$." + std::string(section));
}

bool OneOf(std::string_view value, std::initializer_list<std::string_view> allowed) {
  return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}

Status Validate(const Config& config, const std::filesystem::path& base_directory) {
  constexpr auto kMaxMilliseconds =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (config.server.listen_address.empty())
    return Invalid("$.server.listen_address", "must not be empty");
  in_addr listen_address{};
  if (::inet_pton(AF_INET, config.server.listen_address.c_str(), &listen_address) != 1)
    return Invalid("$.server.listen_address", "must be a valid IPv4 address");
  if (config.server.port == 0) return Invalid("$.server.port", "must be in range 1..65535");
  if (config.server.worker_threads == 0)
    return Invalid("$.server.worker_threads", "must be positive");
  if (config.server.max_connections == 0 || config.server.max_inflight_requests == 0) {
    return Invalid("$.server", "connection and inflight limits must be positive");
  }
  if (config.server.graceful_shutdown_ms == 0) {
    return Invalid("$.server.graceful_shutdown_ms", "must be positive");
  }
  if (config.server.idle_timeout_ms == 0 || config.server.handshake_timeout_ms == 0 ||
      config.server.idle_timeout_ms > kMaxMilliseconds ||
      config.server.handshake_timeout_ms > kMaxMilliseconds ||
      config.server.graceful_shutdown_ms > kMaxMilliseconds) {
    return Invalid("$.server", "connection timeouts must be positive");
  }
  if (config.server.max_input_buffer_bytes == 0 || config.server.max_output_buffer_bytes == 0 ||
      config.server.output_high_watermark_bytes == 0 ||
      config.server.output_high_watermark_bytes > config.server.max_output_buffer_bytes) {
    return Invalid("$.server", "buffer limits are invalid");
  }
  if (config.server.max_input_buffer_bytes < config.protocol.max_frame_bytes)
    return Invalid("$.server.max_input_buffer_bytes", "must cover one protocol frame");
  if (config.protocol.enabled.empty()) return Invalid("$.protocol.enabled", "must not be empty");
  for (const auto& protocol : config.protocol.enabled) {
    if (!OneOf(protocol, {"text-kv", "resp2", "batch-v1"})) {
      return Invalid("$.protocol.enabled", "unknown protocol " + protocol);
    }
  }
  if (config.protocol.parse_timeout_ms == 0 ||
      config.protocol.parse_timeout_ms > kMaxMilliseconds) {
    return Invalid("$.protocol.parse_timeout_ms", "must be positive");
  }
  if (config.protocol.max_key_bytes == 0 || config.protocol.max_value_bytes == 0 ||
      config.protocol.max_batch_records == 0) {
    return Invalid("$.protocol", "size limits must be positive");
  }
  if (config.protocol.max_batch_records > config.server.max_inflight_requests)
    return Invalid("$.protocol.max_batch_records", "must not exceed max inflight requests");
  constexpr std::size_t kFrameOverhead = 128;
  const auto maximum = std::numeric_limits<std::size_t>::max();
  if (config.protocol.max_value_bytes > maximum - kFrameOverhead ||
      config.protocol.max_key_bytes > maximum - kFrameOverhead - config.protocol.max_value_bytes ||
      config.protocol.max_frame_bytes <
          kFrameOverhead + config.protocol.max_value_bytes + config.protocol.max_key_bytes) {
    return Invalid("$.protocol.max_frame_bytes", "too small for configured key and value limits");
  }
  constexpr std::size_t kResponseOverhead = 128;
  if (config.server.max_output_buffer_bytes < config.protocol.max_value_bytes + kResponseOverhead)
    return Invalid("$.server.max_output_buffer_bytes", "must cover one maximum response");
  constexpr std::size_t replication_control_reserve = 1024U;
  constexpr std::size_t replication_chunk_overhead = 13U + 25U;
  if (config.server.max_output_buffer_bytes <=
      replication_control_reserve + replication_chunk_overhead) {
    return Invalid("$.server.max_output_buffer_bytes",
                   "must leave room for a replication chunk and control reserve");
  }
  if (!OneOf(config.engine.type, {"array", "rbtree", "hash", "skiplist"})) {
    return Invalid("$.engine.type", "unknown engine");
  }
  if (config.engine.capacity == 0) return Invalid("$.engine.capacity", "must be positive");
  if (config.engine.hash_initial_buckets == 0 || config.engine.hash_max_load_factor <= 0.0 ||
      config.engine.hash_max_load_factor > 1.0) {
    return Invalid("$.engine.hash", "invalid bucket count or load factor");
  }
  if (config.engine.skiplist_max_level == 0 || config.engine.skiplist_max_level > 64 ||
      config.engine.skiplist_probability <= 0.0 || config.engine.skiplist_probability >= 1.0) {
    return Invalid("$.engine.skiplist", "max_level must be 1..64 and probability must be in (0,1)");
  }
  if (!OneOf(config.network.backend, {"epoll", "io_uring", "ntyco"})) {
    return Invalid("$.network.backend", "unknown backend");
  }
  if (config.network.backend != "epoll")
    return Invalid("$.network.backend", "requested backend is not implemented");
  if (config.network.epoll_max_events == 0 || config.network.io_uring_queue_depth == 0 ||
      config.network.ntyco_stack_bytes < 16384) {
    return Invalid("$.network", "backend limits are invalid");
  }
  if (!OneOf(config.persistence.sync_policy, {"always", "everysec", "no"})) {
    return Invalid("$.persistence.sync_policy", "unknown policy");
  }
  if (config.persistence.aof_enabled &&
      (config.persistence.flush_records == 0 || config.persistence.flush_bytes == 0 ||
       config.persistence.flush_interval_ms == 0 ||
       config.persistence.max_aof_bytes < config.persistence.flush_bytes)) {
    return Invalid("$.persistence", "AOF flush thresholds must be positive");
  }
  if (config.persistence.snapshot_enabled && config.persistence.snapshot_interval_s == 0) {
    return Invalid("$.persistence.snapshot_interval_s", "must be positive when enabled");
  }
  if (!config.persistence.mmap_load)
    return Invalid("$.persistence.mmap_load", "non-mmap loading is not implemented");
  if (config.persistence.io_uring_write && !config.persistence.allow_sync_fallback)
    return Invalid("$.persistence.io_uring_write",
                   "io_uring snapshot writing requires explicit synchronous fallback");
  if (config.persistence.snapshot_enabled && config.persistence.snapshot_interval_s != 3600)
    return Invalid("$.persistence.snapshot_interval_s", "background snapshots are not implemented");
  if (config.server.worker_threads != 1)
    return Invalid("$.server.worker_threads",
                   "only the single reactor implementation is available");
  std::filesystem::path persistence_path = config.persistence.directory;
  if (persistence_path.is_relative()) persistence_path = base_directory / persistence_path;
  const auto accessible =
      std::filesystem::exists(persistence_path) ? persistence_path : persistence_path.parent_path();
  if (accessible.empty() || !std::filesystem::exists(accessible) ||
      !std::filesystem::is_directory(accessible) ||
      ::access(accessible.c_str(), W_OK | X_OK) != 0) {
    return Invalid("$.persistence.directory", "directory or parent is not accessible");
  }
  if (!OneOf(config.replication.role, {"primary", "replica"})) {
    return Invalid("$.replication.role", "must be primary or replica");
  }
  if (config.replication.node_id.empty())
    return Invalid("$.replication.node_id", "must not be empty");
  if (config.replication.role == "primary" && config.replication.upstream.has_value()) {
    return Invalid("$.replication.upstream", "primary must not configure upstream");
  }
  if (config.replication.role == "replica" && !config.replication.upstream.has_value()) {
    return Invalid("$.replication.upstream", "replica requires upstream");
  }
  if (config.replication.upstream.has_value()) {
    const auto& upstream = *config.replication.upstream;
    const auto separator = upstream.rfind(':');
    if (separator == std::string::npos || separator == 0 || separator + 1U >= upstream.size())
      return Invalid("$.replication.upstream", "must be IPv4:port");
    in_addr upstream_address{};
    if (::inet_pton(AF_INET, upstream.substr(0, separator).c_str(), &upstream_address) != 1)
      return Invalid("$.replication.upstream", "host must be a valid IPv4 address");
    std::uint32_t port = 0;
    const auto port_text = upstream.substr(separator + 1U);
    const auto parsed =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() ||
        port == 0 || port > std::numeric_limits<std::uint16_t>::max())
      return Invalid("$.replication.upstream", "port must be in range 1..65535");
  }
  if (config.replication.backlog_slots != 1024) {
    return Invalid("$.replication.backlog_slots", "v0.1 requires exactly 1024 slots");
  }
  if (config.replication.handshake_timeout_ms == 0 ||
      config.replication.heartbeat_interval_ms == 0 ||
      config.replication.handshake_timeout_ms > kMaxMilliseconds ||
      config.replication.heartbeat_interval_ms > kMaxMilliseconds / 3U) {
    return Invalid("$.replication", "handshake and heartbeat intervals must be positive");
  }
  if (!OneOf(config.replication.backend, {"pthread", "reactor", "proactor", "ntyco"})) {
    return Invalid("$.replication.backend", "unknown backend");
  }
  if (config.kvcache.memory_budget_bytes == 0 || config.kvcache.disk_budget_bytes == 0 ||
      config.kvcache.chunk_bytes == 0 || config.kvcache.max_concurrent_loads == 0 ||
      config.kvcache.load_timeout_ms == 0) {
    return Invalid("$.kvcache", "budgets, chunk size, concurrency, and timeout must be positive");
  }
  if (!(config.kvcache.low_watermark > 0.0 &&
        config.kvcache.low_watermark < config.kvcache.high_watermark &&
        config.kvcache.high_watermark < 1.0)) {
    return Invalid("$.kvcache", "watermarks must satisfy 0 < low < high < 1");
  }
  if (!OneOf(config.kvcache.match_policy, {"exact", "longest-prefix"}) ||
      !OneOf(config.kvcache.admission_policy, {"lru", "gdsf"}) ||
      !OneOf(config.kvcache.eviction_policy, {"lru", "gdsf"})) {
    return Invalid("$.kvcache", "unknown match, admission, or eviction policy");
  }
  if (config.kvcache.max_pending_loads == 0 || config.kvcache.max_inflight_io_bytes == 0 ||
      config.kvcache.max_policy_scan == 0 || config.kvcache.tenant_quantum == 0 ||
      config.kvcache.max_tracked_objects == 0) {
    return Invalid("$.kvcache", "scheduler limits must be positive");
  }
  if (!std::isfinite(config.kvcache.prefill_weight) ||
      !std::isfinite(config.kvcache.decode_weight) ||
      !std::isfinite(config.kvcache.low_reuse_weight) || config.kvcache.prefill_weight < 0.0 ||
      config.kvcache.decode_weight < 0.0 || config.kvcache.low_reuse_weight < 0.0) {
    return Invalid("$.kvcache", "workload weights must be finite and non-negative");
  }
  if (!OneOf(config.observability.log_level, {"debug", "info", "warning", "error"})) {
    return Invalid("$.observability.log_level", "unknown level");
  }
  if (config.observability.metrics_enabled &&
      (config.observability.metrics_port == 0 || config.observability.metrics_address.empty())) {
    return Invalid("$.observability",
                   "metrics address must be non-empty and port must be 1..65535");
  }
  if (config.observability.metrics_enabled) {
    in_addr metrics_address{};
    if (::inet_pton(AF_INET, config.observability.metrics_address.c_str(), &metrics_address) != 1)
      return Invalid("$.observability.metrics_address", "must be a valid IPv4 address");
  }
  return Status::Ok();
}

#define KVSTORE_GET(section, field, type)                     \
  auto section##_##field = Get<type>(root, #section, #field); \
  if (!section##_##field.ok()) return section##_##field.status()

Result<Config> Decode(const Json& root, const std::filesystem::path& base_directory) {
  auto status = CheckKeys(root, "$",
                          {"server", "protocol", "engine", "network", "persistence", "replication",
                           "kvcache", "observability"});
  if (!status.ok()) return status;
  for (const auto section : {"server", "protocol", "engine", "network", "persistence",
                             "replication", "kvcache", "observability"}) {
    if (!root.contains(section))
      return Invalid("$." + std::string(section), "required section is missing");
  }

  status = CheckKeys(
      root.at("server"), "$.server",
      {"listen_address", "port", "worker_threads", "max_connections", "max_inflight_requests",
       "graceful_shutdown_ms", "idle_timeout_ms", "handshake_timeout_ms", "max_input_buffer_bytes",
       "max_output_buffer_bytes", "output_high_watermark_bytes"});
  if (!status.ok()) return status;
  status = CheckKeys(root.at("protocol"), "$.protocol",
                     {"enabled", "max_key_bytes", "max_value_bytes", "max_frame_bytes",
                      "max_batch_records", "parse_timeout_ms"});
  if (!status.ok()) return status;
  status =
      CheckKeys(root.at("engine"), "$.engine", {"type", "capacity", "array", "hash", "skiplist"});
  if (!status.ok()) return status;
  for (const auto child : {"array", "hash", "skiplist"}) {
    if (!root.at("engine").contains(child)) {
      return Invalid("$.engine." + std::string(child), "required object is missing");
    }
  }
  status = CheckKeys(root.at("engine").at("array"), "$.engine.array", {"reserve"});
  if (!status.ok()) return status;
  status = CheckKeys(root.at("engine").at("hash"), "$.engine.hash",
                     {"initial_buckets", "max_load_factor"});
  if (!status.ok()) return status;
  status = CheckKeys(root.at("engine").at("skiplist"), "$.engine.skiplist",
                     {"max_level", "probability", "seed"});
  if (!status.ok()) return status;
  status = CheckKeys(root.at("network"), "$.network",
                     {"backend", "allow_fallback", "epoll", "io_uring", "ntyco"});
  if (!status.ok()) return status;
  for (const auto child : {"epoll", "io_uring", "ntyco"}) {
    if (!root.at("network").contains(child)) {
      return Invalid("$.network." + std::string(child), "required object is missing");
    }
  }
  status = CheckKeys(root.at("network").at("epoll"), "$.network.epoll", {"max_events"});
  if (!status.ok()) return status;
  status =
      CheckKeys(root.at("network").at("io_uring"), "$.network.io_uring", {"queue_depth", "sqpoll"});
  if (!status.ok()) return status;
  status = CheckKeys(root.at("network").at("ntyco"), "$.network.ntyco", {"stack_bytes"});
  if (!status.ok()) return status;
  status = CheckKeys(root.at("persistence"), "$.persistence",
                     {"directory", "aof_enabled", "flush_records", "flush_bytes", "max_aof_bytes",
                      "flush_interval_ms", "sync_policy", "snapshot_enabled", "snapshot_interval_s",
                      "mmap_load", "io_uring_write", "allow_sync_fallback", "recover_on_start"});
  if (!status.ok()) return status;
  status = CheckKeys(root.at("replication"), "$.replication",
                     {"role", "node_id", "upstream", "backlog_slots", "handshake_timeout_ms",
                      "heartbeat_interval_ms", "backend"});
  if (!status.ok()) return status;
  status =
      CheckKeys(root.at("kvcache"), "$.kvcache",
                {"memory_budget_bytes", "disk_budget_bytes", "high_watermark", "low_watermark",
                 "chunk_bytes", "match_policy", "admission_policy", "eviction_policy",
                 "max_concurrent_loads", "load_timeout_ms", "max_pending_loads",
                 "max_inflight_io_bytes", "max_policy_scan", "tenant_quantum",
                 "max_tracked_objects", "prefill_weight", "decode_weight", "low_reuse_weight"});
  if (!status.ok()) return status;
  status = CheckKeys(
      root.at("observability"), "$.observability",
      {"log_level", "metrics_enabled", "metrics_address", "metrics_port", "tracing_enabled"});
  if (!status.ok()) return status;

  KVSTORE_GET(server, listen_address, std::string);
  KVSTORE_GET(server, port, std::uint16_t);
  KVSTORE_GET(server, worker_threads, std::size_t);
  KVSTORE_GET(server, max_connections, std::size_t);
  KVSTORE_GET(server, max_inflight_requests, std::size_t);
  KVSTORE_GET(server, graceful_shutdown_ms, std::uint64_t);
  KVSTORE_GET(server, idle_timeout_ms, std::uint64_t);
  KVSTORE_GET(server, handshake_timeout_ms, std::uint64_t);
  KVSTORE_GET(server, max_input_buffer_bytes, std::size_t);
  KVSTORE_GET(server, max_output_buffer_bytes, std::size_t);
  KVSTORE_GET(server, output_high_watermark_bytes, std::size_t);
  KVSTORE_GET(protocol, enabled, std::vector<std::string>);
  KVSTORE_GET(protocol, max_key_bytes, std::size_t);
  KVSTORE_GET(protocol, max_value_bytes, std::size_t);
  KVSTORE_GET(protocol, max_frame_bytes, std::size_t);
  KVSTORE_GET(protocol, max_batch_records, std::size_t);
  KVSTORE_GET(protocol, parse_timeout_ms, std::uint64_t);
  KVSTORE_GET(engine, type, std::string);
  KVSTORE_GET(engine, capacity, std::size_t);
  KVSTORE_GET(network, backend, std::string);
  KVSTORE_GET(network, allow_fallback, bool);
  KVSTORE_GET(persistence, directory, std::string);
  KVSTORE_GET(persistence, aof_enabled, bool);
  KVSTORE_GET(persistence, flush_records, std::size_t);
  KVSTORE_GET(persistence, flush_bytes, std::size_t);
  KVSTORE_GET(persistence, max_aof_bytes, std::uint64_t);
  KVSTORE_GET(persistence, flush_interval_ms, std::uint64_t);
  KVSTORE_GET(persistence, sync_policy, std::string);
  KVSTORE_GET(persistence, snapshot_enabled, bool);
  KVSTORE_GET(persistence, snapshot_interval_s, std::uint64_t);
  KVSTORE_GET(persistence, mmap_load, bool);
  KVSTORE_GET(persistence, io_uring_write, bool);
  KVSTORE_GET(persistence, allow_sync_fallback, bool);
  KVSTORE_GET(persistence, recover_on_start, bool);
  KVSTORE_GET(replication, role, std::string);
  KVSTORE_GET(replication, node_id, std::string);
  if (!root.at("replication").contains("upstream")) {
    return Invalid("$.replication.upstream", "required field is missing");
  }
  std::optional<std::string> replication_upstream;
  if (!root.at("replication").at("upstream").is_null()) {
    auto upstream = Required<std::string>(root.at("replication"), "upstream", "$.replication");
    if (!upstream.ok()) return upstream.status();
    replication_upstream = upstream.value();
  }
  KVSTORE_GET(replication, backlog_slots, std::size_t);
  KVSTORE_GET(replication, handshake_timeout_ms, std::uint64_t);
  KVSTORE_GET(replication, heartbeat_interval_ms, std::uint64_t);
  KVSTORE_GET(replication, backend, std::string);
  KVSTORE_GET(kvcache, memory_budget_bytes, std::uint64_t);
  KVSTORE_GET(kvcache, disk_budget_bytes, std::uint64_t);
  KVSTORE_GET(kvcache, high_watermark, double);
  KVSTORE_GET(kvcache, low_watermark, double);
  KVSTORE_GET(kvcache, chunk_bytes, std::size_t);
  KVSTORE_GET(kvcache, match_policy, std::string);
  KVSTORE_GET(kvcache, admission_policy, std::string);
  KVSTORE_GET(kvcache, eviction_policy, std::string);
  KVSTORE_GET(kvcache, max_concurrent_loads, std::size_t);
  KVSTORE_GET(kvcache, load_timeout_ms, std::uint64_t);
  KVSTORE_GET(kvcache, max_pending_loads, std::size_t);
  KVSTORE_GET(kvcache, max_inflight_io_bytes, std::uint64_t);
  KVSTORE_GET(kvcache, max_policy_scan, std::size_t);
  KVSTORE_GET(kvcache, tenant_quantum, std::uint32_t);
  KVSTORE_GET(kvcache, max_tracked_objects, std::size_t);
  KVSTORE_GET(kvcache, prefill_weight, double);
  KVSTORE_GET(kvcache, decode_weight, double);
  KVSTORE_GET(kvcache, low_reuse_weight, double);
  KVSTORE_GET(observability, log_level, std::string);
  KVSTORE_GET(observability, metrics_enabled, bool);
  KVSTORE_GET(observability, metrics_address, std::string);
  KVSTORE_GET(observability, metrics_port, std::uint16_t);
  KVSTORE_GET(observability, tracing_enabled, bool);

  const auto& engine_json = root.at("engine");
  const auto array_reserve =
      Required<std::size_t>(engine_json.at("array"), "reserve", "$.engine.array");
  const auto hash_buckets =
      Required<std::size_t>(engine_json.at("hash"), "initial_buckets", "$.engine.hash");
  const auto hash_load =
      Required<double>(engine_json.at("hash"), "max_load_factor", "$.engine.hash");
  const auto skip_level =
      Required<std::size_t>(engine_json.at("skiplist"), "max_level", "$.engine.skiplist");
  const auto skip_probability =
      Required<double>(engine_json.at("skiplist"), "probability", "$.engine.skiplist");
  const auto skip_seed =
      Required<std::uint64_t>(engine_json.at("skiplist"), "seed", "$.engine.skiplist");
  const auto& network_json = root.at("network");
  const auto epoll_events =
      Required<std::size_t>(network_json.at("epoll"), "max_events", "$.network.epoll");
  const auto uring_depth =
      Required<std::size_t>(network_json.at("io_uring"), "queue_depth", "$.network.io_uring");
  const auto uring_sqpoll =
      Required<bool>(network_json.at("io_uring"), "sqpoll", "$.network.io_uring");
  const auto ntyco_stack =
      Required<std::size_t>(network_json.at("ntyco"), "stack_bytes", "$.network.ntyco");
  for (const auto* result : {&array_reserve, &hash_buckets, &skip_level, &ntyco_stack}) {
    if (!result->ok()) return result->status();
  }
  if (!hash_load.ok()) return hash_load.status();
  if (!skip_probability.ok()) return skip_probability.status();
  if (!skip_seed.ok()) return skip_seed.status();
  if (!epoll_events.ok()) return epoll_events.status();
  if (!uring_depth.ok()) return uring_depth.status();
  if (!uring_sqpoll.ok()) return uring_sqpoll.status();

  Config config{
      .server = {server_listen_address.value(), server_port.value(), server_worker_threads.value(),
                 server_max_connections.value(), server_max_inflight_requests.value(),
                 server_graceful_shutdown_ms.value(), server_idle_timeout_ms.value(),
                 server_handshake_timeout_ms.value(), server_max_input_buffer_bytes.value(),
                 server_max_output_buffer_bytes.value(),
                 server_output_high_watermark_bytes.value()},
      .protocol = {protocol_enabled.value(), protocol_max_key_bytes.value(),
                   protocol_max_value_bytes.value(), protocol_max_frame_bytes.value(),
                   protocol_max_batch_records.value(), protocol_parse_timeout_ms.value()},
      .engine = {engine_type.value(), engine_capacity.value(), array_reserve.value(),
                 hash_buckets.value(), hash_load.value(), skip_level.value(),
                 skip_probability.value(), skip_seed.value()},
      .network = {network_backend.value(), network_allow_fallback.value(), epoll_events.value(),
                  uring_depth.value(), uring_sqpoll.value(), ntyco_stack.value()},
      .persistence = {persistence_directory.value(), persistence_aof_enabled.value(),
                      persistence_flush_records.value(), persistence_flush_bytes.value(),
                      persistence_max_aof_bytes.value(), persistence_flush_interval_ms.value(),
                      persistence_sync_policy.value(), persistence_snapshot_enabled.value(),
                      persistence_snapshot_interval_s.value(), persistence_mmap_load.value(),
                      persistence_io_uring_write.value(), persistence_allow_sync_fallback.value(),
                      persistence_recover_on_start.value()},
      .replication = {replication_role.value(), replication_node_id.value(), replication_upstream,
                      replication_backlog_slots.value(), replication_handshake_timeout_ms.value(),
                      replication_heartbeat_interval_ms.value(), replication_backend.value()},
      .kvcache = {kvcache_memory_budget_bytes.value(), kvcache_disk_budget_bytes.value(),
                  kvcache_high_watermark.value(), kvcache_low_watermark.value(),
                  kvcache_chunk_bytes.value(), kvcache_match_policy.value(),
                  kvcache_admission_policy.value(), kvcache_eviction_policy.value(),
                  kvcache_max_concurrent_loads.value(), kvcache_load_timeout_ms.value(),
                  kvcache_max_pending_loads.value(), kvcache_max_inflight_io_bytes.value(),
                  kvcache_max_policy_scan.value(), kvcache_tenant_quantum.value(),
                  kvcache_max_tracked_objects.value(), kvcache_prefill_weight.value(),
                  kvcache_decode_weight.value(), kvcache_low_reuse_weight.value()},
      .observability = {observability_log_level.value(), observability_metrics_enabled.value(),
                        observability_metrics_address.value(), observability_metrics_port.value(),
                        observability_tracing_enabled.value()}};
  status = Validate(config, base_directory);
  if (!status.ok()) return status;
  return config;
}

#undef KVSTORE_GET

}  // namespace

Result<Config> Config::Load(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) return Status{StatusCode::kIoError, path.string() + ": cannot open config"};
  std::ostringstream contents;
  contents << input.rdbuf();
  if (!input.good() && !input.eof()) {
    return Status{StatusCode::kIoError, path.string() + ": cannot read config"};
  }
  const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path{"."};
  return Parse(contents.str(), parent);
}

Result<Config> Config::Parse(std::string_view json_text,
                             const std::filesystem::path& base_directory) {
  try {
    return Decode(Json::parse(json_text), base_directory);
  } catch (const Json::parse_error& error) {
    return Invalid("$", error.what());
  } catch (const Json::exception& error) {
    return Invalid("$", error.what());
  }
}

std::string Config::ToRedactedJson() const {
  Json output{
      {"server",
       {{"listen_address", server.listen_address},
        {"port", server.port},
        {"worker_threads", server.worker_threads},
        {"max_connections", server.max_connections},
        {"max_inflight_requests", server.max_inflight_requests},
        {"graceful_shutdown_ms", server.graceful_shutdown_ms},
        {"idle_timeout_ms", server.idle_timeout_ms},
        {"handshake_timeout_ms", server.handshake_timeout_ms},
        {"max_input_buffer_bytes", server.max_input_buffer_bytes},
        {"max_output_buffer_bytes", server.max_output_buffer_bytes},
        {"output_high_watermark_bytes", server.output_high_watermark_bytes}}},
      {"protocol",
       {{"enabled", protocol.enabled},
        {"max_key_bytes", protocol.max_key_bytes},
        {"max_value_bytes", protocol.max_value_bytes},
        {"max_frame_bytes", protocol.max_frame_bytes},
        {"max_batch_records", protocol.max_batch_records},
        {"parse_timeout_ms", protocol.parse_timeout_ms}}},
      {"engine",
       {{"type", engine.type},
        {"capacity", engine.capacity},
        {"array", {{"reserve", engine.array_reserve}}},
        {"hash",
         {{"initial_buckets", engine.hash_initial_buckets},
          {"max_load_factor", engine.hash_max_load_factor}}},
        {"skiplist",
         {{"max_level", engine.skiplist_max_level},
          {"probability", engine.skiplist_probability},
          {"seed", engine.skiplist_seed}}}}},
      {"network",
       {{"backend", network.backend},
        {"allow_fallback", network.allow_fallback},
        {"epoll", {{"max_events", network.epoll_max_events}}},
        {"io_uring",
         {{"queue_depth", network.io_uring_queue_depth}, {"sqpoll", network.io_uring_sqpoll}}},
        {"ntyco", {{"stack_bytes", network.ntyco_stack_bytes}}}}},
      {"persistence",
       {{"directory", persistence.directory.string()},
        {"aof_enabled", persistence.aof_enabled},
        {"flush_records", persistence.flush_records},
        {"flush_bytes", persistence.flush_bytes},
        {"max_aof_bytes", persistence.max_aof_bytes},
        {"flush_interval_ms", persistence.flush_interval_ms},
        {"sync_policy", persistence.sync_policy},
        {"snapshot_enabled", persistence.snapshot_enabled},
        {"snapshot_interval_s", persistence.snapshot_interval_s},
        {"mmap_load", persistence.mmap_load},
        {"io_uring_write", persistence.io_uring_write},
        {"allow_sync_fallback", persistence.allow_sync_fallback},
        {"recover_on_start", persistence.recover_on_start}}},
      {"replication",
       {{"role", replication.role},
        {"node_id", replication.node_id},
        {"upstream",
         replication.upstream.has_value() ? Json(*replication.upstream) : Json(nullptr)},
        {"backlog_slots", replication.backlog_slots},
        {"handshake_timeout_ms", replication.handshake_timeout_ms},
        {"heartbeat_interval_ms", replication.heartbeat_interval_ms},
        {"backend", replication.backend}}},
      {"kvcache",
       {{"memory_budget_bytes", kvcache.memory_budget_bytes},
        {"disk_budget_bytes", kvcache.disk_budget_bytes},
        {"high_watermark", kvcache.high_watermark},
        {"low_watermark", kvcache.low_watermark},
        {"chunk_bytes", kvcache.chunk_bytes},
        {"match_policy", kvcache.match_policy},
        {"admission_policy", kvcache.admission_policy},
        {"eviction_policy", kvcache.eviction_policy},
        {"max_concurrent_loads", kvcache.max_concurrent_loads},
        {"load_timeout_ms", kvcache.load_timeout_ms},
        {"max_pending_loads", kvcache.max_pending_loads},
        {"max_inflight_io_bytes", kvcache.max_inflight_io_bytes},
        {"max_policy_scan", kvcache.max_policy_scan},
        {"tenant_quantum", kvcache.tenant_quantum},
        {"max_tracked_objects", kvcache.max_tracked_objects},
        {"prefill_weight", kvcache.prefill_weight},
        {"decode_weight", kvcache.decode_weight},
        {"low_reuse_weight", kvcache.low_reuse_weight}}},
      {"observability",
       {{"log_level", observability.log_level},
        {"metrics_enabled", observability.metrics_enabled},
        {"metrics_address", observability.metrics_address},
        {"metrics_port", observability.metrics_port},
        {"tracing_enabled", observability.tracing_enabled}}}};
  return output.dump(2);
}

}  // namespace kvstore
