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

The `proactor` and `ntyco` rows currently measure the configured project adapters. They are not
evidence of a real io_uring or third-party NtyCo runtime; those integrations remain explicitly
unsupported until their dependencies and ownership contracts are implemented.
