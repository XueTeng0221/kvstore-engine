#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "kvstore/engine/engine_factory.hpp"

namespace {

using Clock = std::chrono::steady_clock;

std::size_t ResidentBytes() {
  std::ifstream input("/proc/self/statm");
  std::size_t total_pages = 0;
  std::size_t resident_pages = 0;
  input >> total_pages >> resident_pages;
  static_cast<void>(total_pages);
  const long page_size = ::sysconf(_SC_PAGESIZE);
  if (!input || page_size <= 0) return 0;
  return resident_pages * static_cast<std::size_t>(page_size);
}

double Percentile(std::vector<double> values, double percentile) {
  std::sort(values.begin(), values.end());
  const auto index = static_cast<std::size_t>(percentile * static_cast<double>(values.size() - 1));
  return values[index];
}

int Run(std::string_view type, std::size_t value_bytes, int read_percent, int round,
        const std::optional<std::string>& output_path) {
  constexpr std::size_t kKeys = 4000;
  constexpr int kWarmup = 10000;
  constexpr int kOperations = 50000;
  auto engine_result = kvstore::CreateEngine(type, kKeys + 1);
  if (!engine_result.ok()) return 1;
  auto& engine = engine_result.value();
  const std::string value(value_bytes, 'x');
  std::vector<double> load_latency_us;
  load_latency_us.reserve(kKeys);
  for (std::size_t index = 0; index < kKeys; ++index) {
    const auto operation_start = Clock::now();
    if (!engine->Create("key-" + std::to_string(index), value).ok()) return 1;
    load_latency_us.push_back(
        std::chrono::duration<double, std::micro>(Clock::now() - operation_start).count());
  }
  for (int operation = 0; operation < kWarmup; ++operation) {
    const auto key = "key-" + std::to_string(static_cast<std::size_t>(operation) % kKeys);
    if (operation % 100 < read_percent) {
      static_cast<void>(engine->Get(key));
    } else {
      static_cast<void>(engine->Upsert(key, value));
    }
  }

  std::vector<double> latency_us;
  latency_us.reserve(kOperations);
  const auto start = Clock::now();
  for (int operation = 0; operation < kOperations; ++operation) {
    const auto key = "key-" + std::to_string(static_cast<std::size_t>(operation) % kKeys);
    const auto operation_start = Clock::now();
    if (operation % 100 < read_percent) {
      if (!engine->Get(key).ok()) return 1;
    } else if (!engine->Upsert(key, value).ok()) {
      return 1;
    }
    latency_us.push_back(
        std::chrono::duration<double, std::micro>(Clock::now() - operation_start).count());
  }
  const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
  std::ostringstream line;
  line << "round=" << round << " engine=" << type << " keys=" << kKeys
       << " value_bytes=" << value_bytes << " read_percent=" << read_percent
       << " ops_per_sec=" << static_cast<double>(kOperations) / elapsed
       << " p50_us=" << Percentile(latency_us, 0.50) << " p95_us=" << Percentile(latency_us, 0.95)
       << " p99_us=" << Percentile(latency_us, 0.99)
       << " load_p99_us=" << Percentile(load_latency_us, 0.99) << " rss_bytes=" << ResidentBytes()
       << '\n';
  std::cout << line.str();
  if (output_path.has_value()) {
    std::ofstream output(*output_path, std::ios::app);
    if (!output) return 1;
    output << line.str();
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 2) {
    std::cerr << "Usage: engine_benchmark [result-file]\n";
    return 2;
  }
  const std::optional<std::string> output_path =
      argc == 2 ? std::optional<std::string>(argv[1]) : std::nullopt;
  if (output_path.has_value()) {
    std::ofstream output(*output_path, std::ios::trunc);
    if (!output) return 1;
  }
  for (int round = 1; round <= 10; ++round) {
    for (const std::string_view engine : {"array", "rbtree", "hash", "skiplist"}) {
      for (const std::size_t value_bytes : {64U, 1024U, 16384U}) {
        for (const int read_percent : {50, 80, 100}) {
          const pid_t child = ::fork();
          if (child < 0) return 1;
          if (child == 0) {
            const int result = Run(engine, value_bytes, read_percent, round, output_path);
            std::cout.flush();
            ::_exit(result);
          }
          int status = 0;
          if (::waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
              WEXITSTATUS(status) != 0) {
            return 1;
          }
        }
      }
    }
  }
  return 0;
}
