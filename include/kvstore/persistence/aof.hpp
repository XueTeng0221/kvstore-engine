#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <mutex>

#include "kvstore/command/dispatcher.hpp"
#include "kvstore/common/resources.hpp"

namespace kvstore {

class AofWriter {
 public:
  AofWriter(std::filesystem::path directory, std::size_t record_limit, std::size_t byte_limit,
            std::uint64_t interval_ms, std::string sync_policy,
            std::uint64_t max_file_bytes = std::numeric_limits<std::uint64_t>::max());
  ~AofWriter();
  [[nodiscard]] Status Append(const WriteEvent& event);
  [[nodiscard]] Status AppendBatch(const std::vector<WriteEvent>& events);
  [[nodiscard]] Status Flush(bool force_sync = false);
  [[nodiscard]] Result<std::uint64_t> Replay(IEngine& engine, std::uint64_t minimum_offset = 0,
                                             std::uint64_t minimum_event_id = 0);
  [[nodiscard]] std::uint64_t last_event_id() const noexcept { return last_event_id_; }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
  std::size_t record_limit_;
  std::size_t byte_limit_;
  std::uint64_t interval_ms_;
  std::string sync_policy_;
  std::uint64_t max_file_bytes_;
  UniqueFd output_;
  std::mutex mutex_;
  std::size_t pending_records_{0};
  std::size_t pending_bytes_{0};
  std::chrono::steady_clock::time_point last_flush_;
  std::uint64_t last_event_id_{0};
  std::uint64_t last_offset_{0};
  bool failed_{false};
  OwnedThread sync_thread_;
};

}  // namespace kvstore
