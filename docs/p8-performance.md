# P8 Framework Performance

The reproducible raw request samples are `benchmarks/p8-*.json`; external-run
samples record `uds_path_configured=true` and `external_cache_required=true`
(the intentionally local `p8-vllm-hit.json` baseline records false), plus a
`provenance` object containing the git revision, host, framework version, runner
argv, model path/file metadata and UDS configuration. External samples include
the UDS path; local baselines explicitly record that no UDS was configured. Real PyTorch
profiler traces are under `benchmarks/p8-profiles/`, and the machine-readable
summary is `benchmarks/p8-summary.json`. The run used Qwen2.5-0.5B revision
`060db6499f32faf8b98477b0a26969ef7d8b9987`, one RTX 5090 (32,607 MiB), driver
580.76.05, CUDA 13.0, concurrency 1, 470-token cold/full prompts, eight output
tokens, one warmup and ten measured requests per mode.

| Framework | Mode | mean/p50/p95/p99 ms | TTFT ms | TPOT ms | QPS | H/T | GPU activity save | profiler FLOPs save |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| vLLM 0.29.0 | cold miss | 799.93/790.14/867.63/907.59 | 631.72 | 24.03 | 1.250 | 0.00% | 0.00% | 0.00% |
| vLLM 0.29.0 | memory prefix hit | 255.38 | 77.76 | 25.38 | 3.916 | 97.68% | -3.11% | 95.62% |
| vLLM 0.29.0 | memory full hit | 268.56 | 72.83 | 27.96 | 3.724 | 98.72% | -1.93% | 96.66% |
| SGLang 0.5.19 | cold miss | 140.65/123.91/215.96/275.00 | 55.93 | 12.10 | 7.110 | 0.19% | 0.00% | 0.00% |
| SGLang 0.5.19 | memory prefix hit | 144.72 | 60.51 | 12.03 | 6.910 | 99.71% | 11.19% | 97.11% |
| SGLang 0.5.19 | memory full hit | 136.01/119.86/208.18/262.47 | 51.98 | 12.00 | 7.352 | 99.79% | 11.33% | 97.19% |

Prefix-hit rows and exact variance values remain in `p8-summary.json`. vLLM's
OpenAI response does not expose external-token counts, so its `H` is the
connector's accepted full-block count (464 of 470); SGLang reports its hit count
directly. FLOPs are derived from profiler-recorded `aten::mm`, `aten::bmm`, and
`aten::addmm` input shapes because PyTorch 2.13 records shapes but omits a FLOPs
field from Chrome traces. GPU activity is summed from profiler kernel, memcpy,
and memset events and includes decode and external-copy work; it is not presented
as pure prefill kernel time.

The UDS bridge now has an opt-in `TieredStore` disk backend. The ten-run vLLM
disk-hit artifact is `benchmarks/p8-vllm-external-disk.json` and its raw audit
records are `benchmarks/p8-vllm-external-disk.external.jsonl`: all ten measured
requests reported `disk_hit=true`, 114,032,640 disk bytes, and 119,771,740 UDS
bytes in aggregate. Its mean/p50/p95/p99 latency was 652.737/586.668/1035.623/
1309.322 ms, QPS 1.532, and process CPU 26.96%. The disk row is associated with
the stored `p8-profiles/vllm-disk` traces: 2 trace files, 10.324 ms GPU activity
per request and 114,816,450,560 GEMM FLOPs, yielding 96.66% FLOPs savings versus
the vLLM cold baseline. Missing profiler counters remain null rather than
inferred.

SGLang currently produces publication records but no stock prefetch read in the
same disk workload; a real ten-run `disk_hit` probe failed closed with zero
external audit records. Therefore no SGLang disk-hit row is claimed. The stock
0.5.19 cancellation bridge now forwards `PrefetchOperation.is_terminated` as a
real callback, but a framework disk-read trace still requires a workload that
causes the stock prefetch queue to evict/reload pages. Existing SGLang rows
remain ten-run profiler-backed memory baseline/prefix/full measurements. CPU,
network, and disk fields are null for those historical records because the
runner did not receive external audit counters; the report generator preserves
nulls and never fabricates them.

The full-hit throughput gains are 197.86% for vLLM and 3.41% for SGLang;
compute avoidance is represented by profiler-derived FLOPs reductions above,
not token hit rate alone. `tools/p8_profile_report.py` now includes optional
disk rows and carries CPU/network/disk fields from runner artifacts.

Reproduce each mode with `tools/p8_framework_smoke.py --runs 10 --warmup 1
--profile-dir ... --output ...`, then regenerate the summary with:

```bash
python3 tools/p8_profile_report.py --output benchmarks/p8-summary.json
```
