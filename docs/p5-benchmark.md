# P5 executor benchmark

The shared workload is `replication_executor_benchmark`: a 10% warmup followed by 10 rounds of
100,000 accepted tasks per backend, with identical queue capacity and callback work. All four
backends use an asynchronous task callback contract, and latency is measured from submission to
task-body completion. It reports throughput and task-body latency p50/p95/p99. The raw output is
kept in `benchmarks/p5-current.txt`.

Environment used for the current result:

- Linux 6.18.33.2-microsoft-standard-WSL2, x86_64
- GCC 13.3.0
- Debug build, C++20, `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror`
- Command: `./build/replication_executor_benchmark benchmarks/p5-current.txt`

The `io_uring` row is created by the raw Linux io_uring SQE/CQE executor, using a real eventfd
read operation per accepted task. If the host rejects `io_uring_setup`, the raw file records
`status=unavailable` and no throughput is reported. The `ntyco` row runs the pinned third-party
NtyCo C runtime through its coroutine ABI; it is not the old project-owned worker adapter.
Results from unavailable backends must not be compared as zero-throughput measurements.

Current raw result: `benchmarks/p5-current.txt`. The current host accepted both real backends.
