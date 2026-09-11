#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "kvstore/common/result.hpp"

namespace kvstore {

struct ServerConfig {
  std::string listen_address;
  std::uint16_t port{};
  std::size_t worker_threads{};
  std::size_t max_connections{};
  std::size_t max_inflight_requests{};
  std::uint64_t graceful_shutdown_ms{};
  std::uint64_t idle_timeout_ms{};
  std::uint64_t handshake_timeout_ms{};
  std::size_t max_input_buffer_bytes{};
  std::size_t max_output_buffer_bytes{};
  std::size_t output_high_watermark_bytes{};
};

struct ProtocolConfig {
  std::vector<std::string> enabled;
  std::size_t max_key_bytes{};
  std::size_t max_value_bytes{};
  std::size_t max_frame_bytes{};
  std::size_t max_batch_records{};
  std::uint64_t parse_timeout_ms{};
};

struct EngineConfig {
  std::string type;
  std::size_t capacity{};
  std::size_t array_reserve{};
  std::size_t hash_initial_buckets{};
  double hash_max_load_factor{};
  std::size_t skiplist_max_level{};
  double skiplist_probability{};
  std::uint64_t skiplist_seed{};
};

struct NetworkConfig {
  std::string backend;
  bool allow_fallback{};
  std::size_t epoll_max_events{};
  std::size_t io_uring_queue_depth{};
  bool io_uring_sqpoll{};
  std::size_t ntyco_stack_bytes{};
};

struct PersistenceConfig {
  std::filesystem::path directory;
  bool aof_enabled{};
  std::size_t flush_records{};
  std::size_t flush_bytes{};
  std::uint64_t max_aof_bytes{};
  std::uint64_t flush_interval_ms{};
  std::string sync_policy;
  bool snapshot_enabled{};
  std::uint64_t snapshot_interval_s{};
  bool mmap_load{};
  bool io_uring_write{};
  bool allow_sync_fallback{};
  bool recover_on_start{};
};

struct ReplicationConfig {
  std::string role;
  std::string node_id;
  std::optional<std::string> upstream;
  std::size_t backlog_slots{};
  std::uint64_t handshake_timeout_ms{};
  std::uint64_t heartbeat_interval_ms{};
  std::string backend;
};

struct KvCacheConfig {
  std::uint64_t memory_budget_bytes{};
  std::uint64_t disk_budget_bytes{};
  double high_watermark{};
  double low_watermark{};
  std::size_t chunk_bytes{};
  std::string match_policy;
  std::string admission_policy;
  std::string eviction_policy;
  std::size_t max_concurrent_loads{};
  std::uint64_t load_timeout_ms{};
};

struct ObservabilityConfig {
  std::string log_level;
  bool metrics_enabled{};
  std::string metrics_address;
  std::uint16_t metrics_port{};
  bool tracing_enabled{};
};

struct Config {
  ServerConfig server;
  ProtocolConfig protocol;
  EngineConfig engine;
  NetworkConfig network;
  PersistenceConfig persistence;
  ReplicationConfig replication;
  KvCacheConfig kvcache;
  ObservabilityConfig observability;

  [[nodiscard]] static Result<Config> Load(const std::filesystem::path& path);
  [[nodiscard]] static Result<Config> Parse(std::string_view json_text,
                                            const std::filesystem::path& base_directory = ".");
  [[nodiscard]] std::string ToRedactedJson() const;
};

}  // namespace kvstore
