#!/usr/bin/env python3
"""Run a bounded local framework startup and one generation request."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import signal
import re
import socket
import statistics
import subprocess
import sys
import queue
import threading
import time
import urllib.request
from datetime import datetime, timezone
from importlib import metadata
from pathlib import Path


MODEL = os.path.abspath("artifacts/models/Qwen2.5-0.5B")
PYTHON_ROOT = str(Path(__file__).resolve().parents[1] / "python")


def framework_environment() -> dict[str, str]:
    """Make the project adapters importable in framework worker processes."""
    pythonpath = os.environ.get("PYTHONPATH", "")
    entries = [PYTHON_ROOT]
    if pythonpath:
        entries.append(pythonpath)
    return {**os.environ, "PYTHONPATH": os.pathsep.join(entries)}


def request(url: str, payload: dict | None = None) -> dict:
    data = None if payload is None else json.dumps(payload).encode()
    headers = {} if data is None else {"Content-Type": "application/json"}
    req = urllib.request.Request(url, data=data, headers=headers)
    with urllib.request.urlopen(req, timeout=90) as response:
        return json.loads(response.read())


def control(url: str, payload: dict | None = None) -> None:
    data = json.dumps(payload or {}).encode()
    headers = {"Content-Type": "application/json"}
    with urllib.request.urlopen(urllib.request.Request(url, data=data, headers=headers),
                                timeout=120) as response:
        if response.status != 200:
            raise RuntimeError(f"profile control returned {response.status}")


def stream_request(url: str, payload: dict, timeout: float = 90):
    body = dict(payload)
    body["stream"] = True
    if "/v1/" in url:
        body["stream_options"] = {"include_usage": True}
    req = urllib.request.Request(url, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    started = time.perf_counter()
    first = None
    final = {}
    content = {}
    with urllib.request.urlopen(req, timeout=timeout) as response:
        for raw in response:
            line = raw.decode().strip()
            if not line.startswith("data:"):
                continue
            encoded = line[5:].strip()
            if encoded == "[DONE]":
                continue
            item = json.loads(encoded)
            if item.get("usage") is not None:
                final["usage"] = item["usage"]
            if item.get("meta_info") is not None:
                final["meta_info"] = item["meta_info"]
            has_token = bool(item.get("text")) or any(
                choice.get("text") for choice in item.get("choices", ()))
            if has_token:
                content = item
            if has_token and first is None:
                first = time.perf_counter()
    ended = time.perf_counter()
    final.update({key: value for key, value in content.items()
                  if key not in ("usage", "meta_info")})
    return final, (first or ended) - started, ended - started


def wait_http(process: subprocess.Popen[str], output: queue.Queue[str], framework: str, port: int) -> str:
    deadline = time.monotonic() + 90
    started = False
    recent: list[str] = []
    while time.monotonic() < deadline:
        if process.poll() is not None:
            detail = "\n".join(recent)
            raise RuntimeError(
                f"framework exited with status {process.returncode}; output:\n{detail}")
        try:
            line = output.get(timeout=0.2)
            recent.append(line.rstrip())
            del recent[:-200]
            match = re.search(r"127\.0\.0\.1:(\d+)", line)
            if match and int(match.group(1)) == port:
                started = True
        except queue.Empty:
            pass
        if not started:
            continue
        try:
            path = "/model_info" if framework == "sglang" else "/health"
            health = f"http://127.0.0.1:{port}{path}"
            with urllib.request.urlopen(health, timeout=2) as response:
                if response.status == 200:
                    return f"http://127.0.0.1:{port}"
        except Exception:
            time.sleep(1)
    detail = "\n".join(recent)
    raise TimeoutError(f"framework health timeout for {framework}; output:\n{detail}")


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def child_cpu_seconds(pid: int) -> float | None:
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().split()
        ticks = os.sysconf("SC_CLK_TCK")
        return (int(fields[13]) + int(fields[14])) / ticks
    except (OSError, ValueError, IndexError):
        return None


def package_version(name: str) -> str | None:
    try:
        return metadata.version(name)
    except metadata.PackageNotFoundError:
        return None


def provenance(framework: str, model: str, command: list[str], uds_path: str | None) -> dict:
    try:
        revision = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        revision = None
    try:
        diff = subprocess.check_output(["git", "diff", "--binary"])
        diff_sha256 = hashlib.sha256(diff).hexdigest()
    except (OSError, subprocess.CalledProcessError):
        diff_sha256 = None
    try:
        tree = subprocess.check_output(["git", "diff", "HEAD", "--binary"])
        status = subprocess.check_output(["git", "status", "--porcelain=v1"])
        untracked = subprocess.check_output(
            ["git", "ls-files", "--others", "--exclude-standard"], text=True)
        untracked_content = bytearray()
        for name in untracked.splitlines():
            path = Path(name)
            if path.is_file():
                untracked_content.extend(name.encode() + b"\0")
                untracked_content.extend(path.read_bytes())
        tree_fingerprint = hashlib.sha256(
            tree + status + bytes(untracked_content)).hexdigest()
        worktree_dirty = bool(status.strip())
    except (OSError, subprocess.CalledProcessError):
        tree_fingerprint = None
        worktree_dirty = None
    model_path = Path(model).resolve()
    model_files = []
    model_sha256 = None
    path = model_path / "model.safetensors"
    if path.is_file():
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
                digest.update(block)
        model_sha256 = digest.hexdigest()
        model_files.append({"name": path.name, "bytes": path.stat().st_size,
                            "sha256": model_sha256})
    return {
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "git_revision": revision,
        "git_worktree_dirty": worktree_dirty,
        "git_diff_sha256": diff_sha256,
        "git_tree_fingerprint": tree_fingerprint,
        "host": platform.node(),
        "platform": platform.platform(),
        "python": sys.version.split()[0],
        "framework": framework,
        "framework_version": package_version("vllm" if framework == "vllm" else "sglang"),
        "model": Path(model).name,
        "model_revision": "060db6499f32faf8b98477b0a26969ef7d8b9987",
        "model_safetensors_sha256": model_sha256,
        "model_path": str(model_path),
        "model_files": model_files,
        "command": command,
        "command_type": "argv",
        "uds_path": str(Path(uds_path).resolve()) if uds_path else None,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--framework", choices=("vllm", "sglang"), required=True)
    parser.add_argument("--model", default=MODEL)
    parser.add_argument("--uds-path", help="P8.1 UDS path for external KVStore operations")
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--warmup", type=int, default=0)
    parser.add_argument("--mode", choices=("cold_miss", "disk_hit",
                                            "memory_prefix_hit", "memory_full_hit"),
                        default="cold_miss")
    parser.add_argument("--profile-dir", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.runs < 1 or args.warmup < 0:
        raise SystemExit("runs must be positive and warmup non-negative")
    if args.mode == "disk_hit" and not args.uds_path:
        raise SystemExit("disk_hit mode requires --uds-path for disk audit evidence")
    model = os.path.abspath(args.model)
    audit_path = (args.output.with_suffix(".external.jsonl").resolve()
                  if args.output and args.uds_path else None)
    if audit_path and audit_path.exists():
        audit_path.unlink()
    if args.framework == "vllm":
        port = free_port()
        env = {**framework_environment(), "VLLM_USE_FLASHINFER_SAMPLER": "0"}
        command = [
            sys.executable,
            "-m",
            "vllm.entrypoints.cli.main",
            "serve",
            model,
            "--host",
            "127.0.0.1",
            "--port",
            str(port),
            "--enforce-eager",
            "--max-model-len",
            "1024",
            "--max-num-seqs",
            "1",
            "--no-enable-prefix-caching",
            "--gpu-memory-utilization",
            "0.25",
            "--disable-hybrid-kv-cache-manager",
            "--attention-backend",
            "TRITON_ATTN",
            "--kv-transfer-config",
            json.dumps({
                "kv_connector": "KVStoreConnector",
                "kv_role": "kv_both",
                "kv_connector_module_path": "kvstore_vllm.connector",
                "kv_connector_extra_config": ({
                    "uds_path": os.path.abspath(args.uds_path),
                    "tenant_id": "default",
                    "model_id": "Qwen2.5-0.5B",
                    "model_revision": "060db6499f32faf8b98477b0a26969ef7d8b9987",
                    "tokenizer_revision": "060db6499f32faf8b98477b0a26969ef7d8b9987",
                    "audit_path": str(audit_path) if audit_path else None,
                } if args.uds_path else {}),
            }),
        ]
        if args.profile_dir:
            command += ["--profiler-config", json.dumps({
                "profiler": "torch",
                "torch_profiler_dir": str(args.profile_dir.resolve()),
                "torch_profiler_with_stack": False,
                "torch_profiler_with_flops": True,
                "torch_profiler_use_gzip": False,
                "torch_profiler_record_shapes": True,
            })]
        endpoint_path = "/v1/completions"
        payload = {"model": model, "prompt": "hello", "max_tokens": 8,
                   "temperature": 0}
    else:
        port = free_port()
        env = framework_environment()
        command = [
            sys.executable,
            "-m",
            "sglang.launch_server",
            "--model-path",
            model,
            "--host",
            "127.0.0.1",
            "--port",
            str(port),
            "--device",
            "cuda",
            "--tp-size",
            "1",
            "--mem-fraction-static",
            "0.25",
            "--max-total-tokens",
            "1024",
            "--max-running-requests",
            "1",
            "--attention-backend",
            "triton",
            "--sampling-backend",
            "pytorch",
            "--cuda-graph-backend-decode",
            "disabled",
            "--cuda-graph-backend-prefill",
            "disabled",
            "--enable-hierarchical-cache",
            "--hicache-host-memory-mode",
            "buffer_only",
            "--hicache-ratio",
            "0.1",
            "--hicache-storage-backend",
            "dynamic",
            "--hicache-storage-backend-extra-config",
            json.dumps({
                "backend_name": "kvstore",
                "module_path": "kvstore_sglang.hicache",
                "class_name": "KVStoreHiCacheStorage",
                "namespace": "qwen-e2e",
                "tenant_id": "default",
                "model_id": "Qwen2.5-0.5B",
                "model_revision": "060db6499f32faf8b98477b0a26969ef7d8b9987",
                "tokenizer_revision": "060db6499f32faf8b98477b0a26969ef7d8b9987",
                **({"uds_path": os.path.abspath(args.uds_path),
                    "audit_path": str(audit_path) if audit_path else None}
                   if args.uds_path else {}),
            }),
        ]
        endpoint_path = "/generate"
        payload = {"text": "hello", "sampling_params": {
            "temperature": 0, "max_new_tokens": 8
        }}

    process = subprocess.Popen(
        command,
        env=env,
        start_new_session=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    output: queue.Queue[str] = queue.Queue()
    process_started = time.monotonic()

    def collect_output() -> None:
        assert process.stdout is not None
        for line in process.stdout:
            output.put(line)

    threading.Thread(target=collect_output, daemon=True).start()
    try:
        base_url = wait_http(process, output, args.framework, port)
        endpoint = base_url + endpoint_path
        cached_prompt = " ".join(f"cold0-{part}" for part in range(96))

        def body(index, warmup=False):
            value = dict(payload)
            if args.mode == "memory_full_hit":
                prompt = cached_prompt
            elif args.mode == "memory_prefix_hit":
                prompt = cached_prompt if warmup else cached_prompt + f" unique-tail-{index}"
            elif args.mode == "disk_hit":
                prompt = cached_prompt
            else:
                prompt = " ".join(f"cold{index}-{part}" for part in range(96))
            if warmup and args.mode == "cold_miss":
                prompt = " ".join(f"warmup{index}-{part}" for part in range(96))
            value["prompt" if args.framework == "vllm" else "text"] = prompt
            return value

        try:
            for index in range(args.warmup):
                request(endpoint, body(index, True))
            if args.profile_dir:
                args.profile_dir.mkdir(parents=True, exist_ok=True)
                profile_body = ({"output_dir": str(args.profile_dir.resolve()),
                                 "activities": ["CPU", "GPU"], "with_stack": False,
                                 "record_shapes": True, "profile_prefix": args.mode}
                                if args.framework == "sglang" else None)
                control(base_url + "/start_profile", profile_body)
            rows = []
            for index in range(args.runs):
                started = time.perf_counter()
                response, ttft, elapsed = stream_request(endpoint, body(index))
                usage = response.get("usage", {})
                if args.framework == "sglang":
                    meta = response.get("meta_info", {})
                    usage = {
                        "prompt_tokens": meta.get("prompt_tokens"),
                        "completion_tokens": meta.get("completion_tokens"),
                        "prompt_tokens_details": {
                            "cached_tokens": meta.get("cached_tokens",
                                                       meta.get("prefix_matched_tokens"))
                        },
                    }
                output_tokens = usage.get("completion_tokens")
                rows.append({"latency_ms": elapsed * 1000.0,
                             "ttft_ms": ttft * 1000.0,
                             "tpot_ms": ((elapsed - ttft) * 1000.0 / (output_tokens - 1)
                                         if output_tokens and output_tokens > 1 else None),
                             "input_tokens": usage.get("prompt_tokens"),
                             "output_tokens": output_tokens,
                             "cached_tokens": (usage.get("prompt_tokens_details") or {}).get(
                                 "cached_tokens")})
            if args.profile_dir:
                control(base_url + "/stop_profile")
        except Exception:
            lines = []
            while True:
                try:
                    lines.append(output.get_nowait())
                except queue.Empty:
                    break
            if lines:
                print("".join(lines[-80:]), file=sys.stderr)
            raise
        if args.framework == "vllm" and not response.get("choices"):
            raise RuntimeError("vLLM returned no choices")
        if args.framework == "sglang" and not response.get("text"):
            raise RuntimeError("SGLang returned no text")
        result = {"framework": args.framework, "mode": args.mode,
                  "runs": args.runs, "warmup": args.warmup, "samples": rows,
                  "uds_path_configured": bool(args.uds_path),
                  "external_cache_required": bool(args.uds_path),
                  "profile_dir": str(args.profile_dir.resolve()) if args.profile_dir else None,
                  "provenance": provenance(args.framework, model, command, args.uds_path)}
        result["provenance"]["runner_command"] = [
            sys.executable, str(Path(__file__).resolve()),
            "--framework", args.framework, "--model", model,
            "--mode", args.mode, "--runs", str(args.runs),
            "--warmup", str(args.warmup),
            *("--uds-path", str(Path(args.uds_path).resolve())
              if args.uds_path else ()),
            *("--profile-dir", str(args.profile_dir.resolve())
              if args.profile_dir else ()),
            *("--output", str(args.output.resolve()) if args.output else ()),
        ]
        latencies = [row["latency_ms"] for row in rows]
        ordered = sorted(latencies)
        result["latency_ms_mean"] = statistics.fmean(latencies)
        middle = len(ordered) // 2
        result["latency_ms_p50"] = (
            ordered[middle] if len(ordered) % 2 else
            (ordered[middle - 1] + ordered[middle]) / 2.0)
        result["qps"] = 1000.0 / result["latency_ms_mean"]
        cpu_seconds = child_cpu_seconds(process.pid)
        result["cpu_percent"] = (
            cpu_seconds / max(1e-9, time.monotonic() - process_started) * 100.0
            if cpu_seconds is not None else None)
        if audit_path:
            if not audit_path.exists():
                raise RuntimeError(
                    "framework configured with external UDS but produced no audit records")
            records = [json.loads(line) for line in audit_path.read_text().splitlines()]
            result["external_operations"] = {
                name: sum(record["operation"] == name for record in records)
                for name in ("lookup", "get_pages", "publish")
            }
            result["external_hit_tokens"] = [
                record["hit_tokens"] for record in records
                if record["operation"] == "get_pages"
            ]
            result["network_bytes"] = max(
                (record.get("network_bytes", 0) for record in records), default=0)
            result["disk_bytes"] = (sum(record["disk_bytes"] for record in records
                                         if isinstance(record.get("disk_bytes"), (int, float)))
                                    if any("disk_bytes" in record for record in records)
                                    else None)
            result["disk_hit"] = any(
                record.get("operation") == "disk_hit" or record.get("disk_hit", False)
                for record in records)
            result["disk_metrics_available"] = any(
                record.get("operation") == "disk_hit" or "disk_hit" in record or
                "disk_bytes" in record
                for record in records)
            if not records:
                raise RuntimeError("external UDS audit contains no operations")
            if args.mode in {"memory_prefix_hit", "memory_full_hit"}:
                hits = [record.get("hit_tokens", 0) for record in records
                        if record.get("operation") == "get_pages"]
                if not hits or max(hits) <= 0:
                    raise RuntimeError(
                        "external cache mode requested but no non-zero get_pages hit was recorded")
                if args.framework == "sglang" and not any(
                        record.get("operation") == "lookup" for record in records):
                    raise RuntimeError("SGLang external audit lacks lookup records")
            if args.mode == "disk_hit" and not result["disk_hit"]:
                raise RuntimeError("disk_hit mode requires a disk_hit audit operation")
        else:
            if args.uds_path:
                raise RuntimeError("external UDS audit path was not configured")
            result["network_bytes"] = 0
            result["disk_bytes"] = None
            result["disk_hit"] = False
            result["disk_metrics_available"] = False
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(result, indent=2) + "\n")
        print(f"{args.framework}_http_ok")
        return 0
    finally:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()


if __name__ == "__main__":
    raise SystemExit(main())
