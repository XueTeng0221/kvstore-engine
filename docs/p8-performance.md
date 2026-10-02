# P8 Framework Performance

The reproducible raw request samples are `benchmarks/p8-*.json`; each sample
records `uds_path_configured=true` and `external_cache_required=true`. Real PyTorch
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

The resident UDS fixture has no disk tier, so no disk-hit number is fabricated.
Disk-hit performance remains a P8.4 limitation until the production bridge is
wired to `TieredStore`. CPU percentage and network byte counters are also not
available from these framework responses. The full-hit throughput gains are
197.86% for vLLM and 3.41% for SGLang; compute avoidance is represented by the
profiler-derived FLOPs reductions above, not token hit rate alone.

Reproduce each mode with `tools/p8_framework_smoke.py --runs 10 --warmup 1
--profile-dir ... --output ...`, then regenerate the summary with:

```bash
python3 tools/p8_profile_report.py --output benchmarks/p8-summary.json
```
