#!/usr/bin/env python3
"""Reproducible P8 request-level performance matrix.

The framework process is supplied by the caller.  This keeps the measurement
driver independent of vLLM/SGLang launch flags while recording the required
cache mode, hit ratio, request latency and optional framework metrics.
"""

from __future__ import annotations

import argparse
import json
import statistics
import time
import urllib.request
from pathlib import Path


def call(url: str, payload: dict, timeout: float) -> tuple[float, dict]:
    request = urllib.request.Request(
        url, data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"})
    started = time.perf_counter()
    with urllib.request.urlopen(request, timeout=timeout) as response:
        body = json.loads(response.read())
    return (time.perf_counter() - started) * 1000.0, body


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = fraction * (len(ordered) - 1)
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    parser.add_argument("--payload", type=Path, required=True)
    parser.add_argument("--modes", default="cold_miss,disk_hit,memory_prefix_hit,memory_full_hit")
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    if args.runs < 1 or args.warmup < 0:
        raise SystemExit("runs must be positive and warmup must be non-negative")
    payload = json.loads(args.payload.read_text())
    modes = [mode.strip() for mode in args.modes.split(",") if mode.strip()]
    rows = []
    for mode in modes:
        request_payload = dict(payload)
        request_payload["kvstore_mode"] = mode
        for _ in range(args.warmup):
            call(args.url, request_payload, args.timeout)
        measured = [call(args.url, request_payload, args.timeout)
                    for _ in range(args.runs)]
        samples = [latency for latency, _ in measured]
        metrics = [body.get("kvstore_metrics", {}) for _, body in measured]
        def metric(name):
            values = [item[name] for item in metrics if isinstance(item.get(name), (int, float))]
            return statistics.fmean(values) if values else None
        rows.append({
            "mode": mode,
            "runs": args.runs,
            "warmup": args.warmup,
            "latency_ms_mean": statistics.fmean(samples),
            "latency_ms_p50": percentile(samples, 0.50),
            "latency_ms_p95": percentile(samples, 0.95),
            "latency_ms_p99": percentile(samples, 0.99),
            "latency_ms_stdev": statistics.stdev(samples) if len(samples) > 1 else 0.0,
            "samples_ms": samples,
            "hit_tokens": metric("hit_tokens"),
            "input_tokens": metric("input_tokens"),
            "ttft_ms": metric("ttft_ms"),
            "tpot_ms": metric("tpot_ms"),
            "qps": metric("qps"),
            "gpu_prefill_ms": metric("gpu_prefill_ms"),
            "prefill_flops": metric("prefill_flops"),
            "disk_bytes": metric("disk_bytes"),
            "network_bytes": metric("network_bytes"),
            "cpu_percent": metric("cpu_percent"),
            "disk_hit": any(item.get("disk_hit") is True for item in metrics),
            "disk_metrics_available": any(
                "disk_bytes" in item or "disk_hit" in item for item in metrics),
        })
    baseline = next((row for row in rows if row["mode"] == "cold_miss"), None)
    for row in rows:
        row["throughput_gain_percent"] = (
            (row["qps"] / baseline["qps"] - 1.0) * 100.0
            if baseline and baseline["qps"] and row["qps"] else None)
        row["compute_savings_percent"] = (
            (baseline["gpu_prefill_ms"] - row["gpu_prefill_ms"])
            / baseline["gpu_prefill_ms"] * 100.0
            if baseline and baseline["gpu_prefill_ms"] and row["gpu_prefill_ms"] is not None
            else None)
        row["recompute_avoidance_percent"] = (
            (row["hit_tokens"] / row["input_tokens"] * 100.0)
            if isinstance(row["hit_tokens"], (int, float)) and row["input_tokens"] else None)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({"rows": rows}, indent=2) + "\n")
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        lines = ["# P8 Performance Matrix", "", "| mode | mean ms | p50 | p95 | p99 | TTFT | TPOT | QPS | H/T | disk hit | disk bytes | CPU | prefill save | throughput gain |", "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
        for row in rows:
            hit = "n/a" if row["recompute_avoidance_percent"] is None else f'{row["recompute_avoidance_percent"]:.2f}%'
            def fmt(name):
                value = row[name]
                return "n/a" if value is None else f"{value:.3f}"
            gain = "n/a" if row["throughput_gain_percent"] is None else f'{row["throughput_gain_percent"]:.2f}%'
            saving = "n/a" if row["compute_savings_percent"] is None else f'{row["compute_savings_percent"]:.2f}%'
            disk = "yes" if row["disk_hit"] else "no"
            lines.append(f'| {row["mode"]} | {row["latency_ms_mean"]:.3f} | {row["latency_ms_p50"]:.3f} | {row["latency_ms_p95"]:.3f} | {row["latency_ms_p99"]:.3f} | {fmt("ttft_ms")} | {fmt("tpot_ms")} | {fmt("qps")} | {hit} | {disk} | {fmt("disk_bytes")} | {fmt("cpu_percent")} | {saving} | {gain} |')
        lines += ["", "The endpoint must return numeric `kvstore_metrics` fields for TTFT, TPOT, QPS, GPU prefill, profiler FLOPs, bytes and CPU; missing counters remain null rather than being fabricated.", ""]
        args.report.write_text("\n".join(lines))
    print(json.dumps({"output": str(args.output), "rows": len(rows)}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
