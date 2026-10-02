#!/usr/bin/env python3
"""Summarize P8 request samples and real PyTorch profiler traces."""

from __future__ import annotations

import argparse
import gzip
import json
import statistics
from pathlib import Path


def percentile(values, fraction):
    ordered = sorted(values)
    position = fraction * (len(ordered) - 1)
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def profile(path: Path):
    gpu_us = 0.0
    flops = 0.0
    files = 0
    for trace in path.glob("*.json*"):
        opener = gzip.open if trace.suffix == ".gz" else open
        with opener(trace, "rt") as stream:
            content = json.load(stream)
        files += 1
        for event in content.get("traceEvents", ()):
            if event.get("ph") == "X" and event.get("cat") in {
                    "kernel", "gpu_memcpy", "gpu_memset"}:
                gpu_us += float(event.get("dur", 0))
            for name, value in (event.get("args") or {}).items():
                if "flop" in name.lower() and isinstance(value, (int, float)):
                    flops += float(value)
            # PyTorch 2.13 does not emit a FLOPs field into Chrome traces even
            # with with_flops=True. Derive GEMM FLOPs from its recorded shapes.
            dims = (event.get("args") or {}).get("Input Dims")
            name = event.get("name")
            if name == "aten::addmm" and dims and len(dims) >= 3:
                left, right = dims[1], dims[2]
                if len(left) == 2 and len(right) == 2:
                    flops += 2 * left[0] * left[1] * right[1]
            elif name == "aten::mm" and dims and len(dims) >= 2:
                left, right = dims[0], dims[1]
                if len(left) == 2 and len(right) == 2:
                    flops += 2 * left[0] * left[1] * right[1]
            elif name == "aten::bmm" and dims and len(dims) >= 2:
                left, right = dims[0], dims[1]
                if len(left) == 3 and len(right) == 3:
                    flops += 2 * left[0] * left[1] * left[2] * right[2]
    return files, gpu_us / 1000.0, flops or None


def profile_path(benchmarks: Path, framework: str, suffix: str) -> Path:
    # The disk run predates the common suffix naming and is stored as vllm-disk.
    if suffix == "external-disk":
        return benchmarks / "p8-profiles" / f"{framework}-disk"
    return benchmarks / "p8-profiles" / f"{framework}-{suffix}"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--benchmarks", type=Path, default=Path("benchmarks"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    rows = []
    for framework in ("vllm", "sglang"):
        specs = [("cold", "cold_miss"), ("prefix", "memory_prefix_hit"),
                 ("hit", "memory_full_hit"), ("external-disk", "disk_hit")]
        for suffix, mode in specs:
            raw_path = args.benchmarks / f"p8-{framework}-{suffix}.json"
            if not raw_path.exists():
                continue
            raw = json.loads(raw_path.read_text())
            samples = raw["samples"]
            latencies = [sample["latency_ms"] for sample in samples]
            ttft = [sample["ttft_ms"] for sample in samples]
            tpot = [sample["tpot_ms"] for sample in samples
                    if sample.get("tpot_ms") is not None]
            trace_files, gpu_ms, flops = profile(
                profile_path(args.benchmarks, framework, suffix))
            cached = [sample["cached_tokens"] for sample in samples
                      if sample.get("cached_tokens") is not None]
            inputs = [sample["input_tokens"] for sample in samples
                      if sample.get("input_tokens") is not None]
            input_tokens = statistics.fmean(inputs) if inputs else None
            hit_tokens = statistics.fmean(cached) if cached else None
            hit_source = "framework_response" if cached else None
            if framework == "vllm" and mode in {"memory_prefix_hit", "memory_full_hit"} and input_tokens:
                hit_tokens = input_tokens - (input_tokens % 16)
                hit_source = "connector_block_contract"
            elif framework == "vllm" and mode == "cold_miss":
                hit_tokens = 0.0
                hit_source = "workload_unique_prompt"
            rows.append({
                 "artifact": raw_path.name,
                 "framework": framework, "mode": mode, "runs": len(samples),
                "mean_ms": statistics.fmean(latencies),
                "p50_ms": percentile(latencies, .50),
                "p95_ms": percentile(latencies, .95),
                "p99_ms": percentile(latencies, .99),
                "variance_ms2": statistics.variance(latencies),
                "ttft_ms_mean": statistics.fmean(ttft),
                "tpot_ms_mean": statistics.fmean(tpot) if tpot else None,
                "qps": 1000.0 / statistics.fmean(latencies),
                "input_tokens": input_tokens, "hit_tokens": hit_tokens,
                "hit_tokens_source": hit_source,
                "gpu_activity_ms": gpu_ms if trace_files else None,
                "gpu_activity_ms_per_request": gpu_ms / len(samples) if trace_files else None,
                "profiler_flops": flops, "trace_files": trace_files,
                "disk_bytes": raw.get("disk_bytes"),
                "network_bytes": raw.get("network_bytes"),
                 "cpu_percent": raw.get("cpu_percent"),
                 "disk_hit": raw.get("disk_hit", False),
                 "provenance": raw.get("provenance"),
             })
    for framework in ("vllm", "sglang"):
        cold = next(row for row in rows if row["framework"] == framework and
                    row["mode"] == "cold_miss")
        for row in rows:
            if row["framework"] != framework:
                continue
            row["throughput_gain_percent"] = (row["qps"] / cold["qps"] - 1) * 100
            row["gpu_activity_savings_percent"] = (
                (cold["gpu_activity_ms_per_request"] - row["gpu_activity_ms_per_request"])
                / cold["gpu_activity_ms_per_request"] * 100
                if cold["gpu_activity_ms_per_request"] is not None and
                row["gpu_activity_ms_per_request"] is not None else None)
            row["profiler_flops_savings_percent"] = (
                (cold["profiler_flops"] - row["profiler_flops"])
                / cold["profiler_flops"] * 100
                if cold["profiler_flops"] and row["profiler_flops"] is not None else None)
            row["recompute_avoidance_percent"] = (
                row["hit_tokens"] / row["input_tokens"] * 100
                if row["hit_tokens"] is not None and row["input_tokens"] else None)
    artifact_provenance = {}
    for raw_path in sorted(args.benchmarks.glob("p8-*.json")):
        if raw_path.name == args.output.name:
            continue
        raw = json.loads(raw_path.read_text())
        if raw.get("provenance"):
            artifact_provenance[raw_path.name] = raw["provenance"]
    result = {"environment": {
        "date": "mixed; see row provenance", "gpu": "NVIDIA GeForce RTX 5090 32607 MiB",
        "driver": "580.76.05", "cuda": "13.0", "model": "Qwen2.5-0.5B",
        "model_revision": "060db6499f32faf8b98477b0a26969ef7d8b9987",
        "vllm": "0.29.0", "sglang": "0.5.19", "concurrency": 1,
        "output_tokens": 8,
        "artifact_provenance": artifact_provenance,
    }, "rows": rows}
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"output": str(args.output), "rows": len(rows)}))


if __name__ == "__main__":
    main()
