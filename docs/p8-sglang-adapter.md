# SGLang adapter (P8.3)

The supported baseline is SGLang `0.5.19` with Qwen2.5-0.5B. This adapter models
the dynamic `HiCacheStorage` `interface_v1` boundary without importing SGLang or
CUDA headers. A radix node is represented by its complete token prefix; its model,
tokenizer, layer, head geometry, dtype, device and block metadata are mapped to
the P8.1 canonical manifest. Consequently tenant/model/revision and tensor layout
cannot accidentally cross a cache entry.

`HiCacheStorage` sends serialized P8.1 requests. Lookup returns miss, partial hit,
or full hit ranges. Publish performs reserve, per-chunk CRC checked PUT, and commit;
any failed PUT aborts the reservation. Release only drops the session lease.
Deadline and cancellation are admission failures and callers must recompute. A
bad GET checksum is a fallback condition, never usable cache data. Reconnects must
negotiate again; published resident objects remain discoverable after a server
restart when the backing server has restored them.

This is a framework-independent contract/mock path. It does not claim Python
HiCacheStorage loading, radix insertion, CUDA DMA, or SGLang/GPU end-to-end tests.

## Runtime validation boundary

For a local Qwen2.5-0.5B smoke run, use the reproducible standard-library
runner and enable hierarchical cache explicitly:

```bash
PATH="$PWD/.venv-sglang/bin:$PATH" PYTHONPATH=python \
  .venv-sglang/bin/python tools/p8_framework_smoke.py \
  --framework sglang --model "$PWD/artifacts/models/Qwen2.5-0.5B"

HF_HUB_OFFLINE=1 VLLM_USE_FLASHINFER_SAMPLER=0 PYTHONPATH=python \
  .venv-vllm/bin/python tools/p8_framework_smoke.py \
  --framework vllm --model "$PWD/artifacts/models/Qwen2.5-0.5B"
```

This proves model loading, backend factory registration, host-pool attachment,
and one framework request (`GET /model_info` plus `POST /generate` for SGLang;
`GET /health` plus `POST /v1/completions` for vLLM) only.
Set `uds_path` in `--hicache-storage-backend-extra-config` to use the P8.1
Protobuf-over-UDS Session. In this mode the backend does not allocate the
process-local `_pages` map: page reads and writes perform Session lookup,
GET/RELEASE, RESERVE/PUT/COMMIT, and ABORT operations against the external
KVStore. The standard-library smoke runner accepts `--uds-path` and passes this
configuration to SGLang. Without `uds_path`, local mode remains available for
framework startup diagnostics only.
