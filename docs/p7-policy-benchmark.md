# P7.4 Deterministic Policy Benchmark

This benchmark is a deterministic policy simulation, not a machine-performance
claim. It counts cache misses, configured recompute cost, and request latency in
model units for the fixed trace documented in `docs/kvcache-policy.md`.

Command:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DKVSTORE_BUILD_TESTS=OFF
cmake --build build-release -j2 --target kvcache_policy_benchmark
./build-release/kvcache_policy_benchmark
```

Result:

```text
policy=lru requests=74 misses=15 miss_cost=1302 deterministic_p95_latency=100
policy=gdsf requests=74 misses=38 miss_cost=137 deterministic_p95_latency=1
```

GDSF deliberately accepts 23 additional cost-1 misses to avoid repeated
cost-100 misses. Total modeled miss cost falls by 89.5%, and deterministic p95
request latency falls from 100 to 1. These are trace-model results and do not
claim wall-clock throughput or latency.
