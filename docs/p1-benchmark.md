# P1 Engine Benchmark Report

## Environment

- Date: 2026-09-09
- Build: GCC 13.3.0, CMake 3.28.3, `Release`, sanitizers disabled
- Host: WSL2 Linux 6.18.33.2, Intel Core i9-13900H, 20 logical CPUs, 24 MiB L3
- Execution: one worker, 4,000 resident keys, 10,000 warmup operations, 50,000 measured operations per case, 10 rounds
- Matrix: 64/1,024/16,384-byte values and 50/80/100 percent reads
- Isolation: every engine/value/read-ratio case runs in a fresh child process
- Final ten-round full matrix: 41.81 seconds wall, 37.14 seconds user, 4.31 seconds system, 69,312 KiB peak RSS; measured with `/usr/bin/time`.
- The benchmark also emits `load_p99_us`; this includes initial allocation, tree balancing, SkipList promotion, and Hash rehash tail latency.

Commands:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DKVSTORE_BUILD_TESTS=OFF -DKVSTORE_BUILD_BENCHMARKS=ON
cmake --build build-release -j2
./build-release/engine_benchmark benchmarks/p1-current.txt
/usr/bin/time -v ./build-release/engine_benchmark >/dev/null
```

## Representative 80/20 Results

The table below is calculated from the final ten-round raw result for the representative 1 KiB value and 80% read workload. Throughput is the min-max range across rounds; latency and RSS are the corresponding observed ranges. The raw file contains the complete 360-case matrix. RSS includes process/runtime overhead, so amplification is a conservative process-RSS/payload ratio rather than allocator-only overhead.

| Engine | Value | Throughput ops/s | p50 us | p95 us | p99 us | RSS | RSS/payload |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Array | 1 KiB | 238,023-332,823 | 2.419-3.347 | 6.123-9.253 | 6.531-13.620 | 9.24 MB | 2.24x |
| RBTree | 1 KiB | 3,322,790-3,776,720 | 0.183-0.193 | 0.295-0.344 | 0.407-0.532 | 9.64 MB | 2.34x |
| Hash | 1 KiB | 4,142,140-4,959,820 | 0.134-0.145 | 0.187-0.242 | 0.238-0.349 | 9.52 MB | 2.31x |
| SkipList | 1 KiB | 2,366,790-2,934,970 | 0.254-0.277 | 0.409-0.574 | 0.530-0.813 | 9.73 MB | 2.36x |

## Decision

Hash is the P1 default because it has the highest throughput and lowest latency for the 64-byte and 1 KiB representative workloads. RBTree and SkipList remain available for ordered scans and future prefix-index experiments. Array remains a compact correctness baseline for small capacities; its linear lookup dominates at 4,000 keys.

Using the final ten-round raw result for the representative 1 KiB 80/20 case, average throughput is 312,028/s for Array, 3,585,157/s for RBTree, 4,498,744/s for Hash, and 2,714,701/s for SkipList. Relative to Hash, throughput regression is 93.1% for Array, 20.3% for RBTree, and 39.7% for SkipList. These are same-workload microbenchmark comparisons, not production performance claims.

The same final raw slice reports average p50/p95/p99 latency of Array `2.524/6.469/7.355 us`, RBTree `0.187/0.318/0.462 us`, Hash `0.138/0.220/0.313 us`, and SkipList `0.260/0.448/0.618 us`. Average RSS is `9.24/9.64/9.52/9.73 MB`; amplification divides that RSS by approximately `4.13 MB` of resident key/value payload and includes process/runtime overhead.

These numbers are comparative development results, not the production target claimed in `architecture.md`. Network, persistence, allocator fragmentation over time, multiple workers, and real Attention KV chunks are not represented and must be measured in later milestones.
