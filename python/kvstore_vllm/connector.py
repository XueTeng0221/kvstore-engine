"""Minimal vLLM KVConnectorBase_V1 bridge.

The scheduler and worker callbacks share a bounded in-process prefix index. The
transport hook is deliberately injected so production deployments can connect
the existing P8.1 Session without making framework code depend on C++ symbols.
"""

from __future__ import annotations

from dataclasses import dataclass
from collections.abc import Iterable
from threading import RLock
from contextvars import ContextVar
from contextlib import contextmanager
from typing import Any, Callable

try:
    from vllm.distributed.kv_transfer.kv_connector.v1.base import (
        KVConnectorBase_V1,
    )
except ImportError as exc:  # pragma: no cover - exercised by environment check
    raise ImportError("requires vLLM 0.29.0 KVConnectorBase_V1") from exc

try:
    from vllm.distributed.kv_transfer.kv_connector.v1.base import (
        KVConnectorMetadata,
        KVConnectorWorkerMetadata,
    )
except ImportError:  # vLLM keeps these as lightweight marker types.
    KVConnectorMetadata = object
    KVConnectorWorkerMetadata = object


@dataclass
class _Entry:
    token_count: int
    payload: Any

@dataclass
class _Save:
    key: tuple[int, ...]
    payload: Any
    generation: int


@dataclass(frozen=True)
class _Load:
    request_id: str
    token_count: int
    block_ids: tuple[int, ...]
    generation: int
    token_ids: tuple[int, ...] = ()


@dataclass
class _Transfer:
    request_id: str
    generation: int
    kind: str
    event: Any
    buffers: list[Any]
    cancelled: bool = False


@dataclass(frozen=True)
class _Metadata(KVConnectorMetadata):
    loads: tuple[_Load, ...] = ()
    saves: tuple[_Load, ...] = ()


class KVStoreConnector(KVConnectorBase_V1):
    """Framework callback implementation with exact and prefix lookup."""

    def __init__(self, vllm_config, role, kv_cache_config):
        super().__init__(vllm_config, role, kv_cache_config)
        self._kv_cache_config = kv_cache_config
        self._entries: dict[tuple[int, ...], _Entry] = {}
        self._pending: dict[str, tuple[int, ...]] = {}
        self._load: dict[str, _Save] = {}
        self._put: Callable[[tuple[int, ...], Any], None] | None = None
        self._lock = RLock()
        self._active_request: ContextVar[str | None] = ContextVar(
            "kvstore_vllm_active_request", default=None
        )
        self._saving: set[str] = set()
        self._finished: set[str] = set()
        self._kv_caches: dict[str, Any] = {}
        self._load_plans: dict[str, _Load] = {}
        self._save_plans: dict[str, _Load] = {}
        self._load_events: dict[str, dict[str, Any]] = {}
        self._transfers: dict[tuple[str, int], _Transfer] = {}
        self._load_errors: set[int] = set()
        self._gpu_block_pool = None
        self._load_stream = None
        self._save_stream = None
        self._transport = None
        self._generation: dict[str, int] = {}
        extra = getattr(getattr(vllm_config, "kv_transfer_config", None),
                        "kv_connector_extra_config", {}) or {}
        factory = extra.get("transport_factory")
        if callable(factory):
            self._transport = factory(extra)
        elif extra.get("uds_path"):
            from .bridge import VllmSessionBridge
            self._transport = VllmSessionBridge.from_vllm_config(
                vllm_config, kv_cache_config, extra)
            self._put = self._transport.publish

    @staticmethod
    def requires_piecewise_for_cudagraph(extra_config: dict[str, Any]) -> bool:
        del extra_config
        return True

    def register_kv_caches(self, kv_caches: dict[str, Any]) -> None:
        """Register the worker's real paged tensors and their CUDA streams."""
        if not kv_caches:
            raise ValueError("vLLM supplied no KV cache tensors")
        config = self._kv_cache_config
        expected = {
            layer_name
            for group in getattr(config, "kv_cache_groups", ())
            for layer_name in group.layer_names
        }
        actual = set(kv_caches)
        if expected and actual != expected:
            missing = sorted(expected - actual)
            extra = sorted(actual - expected)
            raise ValueError(f"KV cache layers do not match runtime config: missing={missing}, extra={extra}")
        import torch
        if any(not isinstance(tensor, torch.Tensor) for tensor in kv_caches.values()):
            raise ValueError("vLLM KV caches must be torch tensors")
        devices = {str(tensor.device) for tensor in kv_caches.values()}
        if len(devices) != 1 or not next(iter(devices)).startswith("cuda"):
            raise ValueError("KVStoreConnector requires one CUDA KV cache device")
        if {str(tensor.dtype) for tensor in kv_caches.values()} != {"torch.bfloat16"}:
            raise ValueError("KVStoreConnector requires bfloat16 KV caches")
        if any(tensor.ndim < 2 or any(size <= 0 for size in tensor.shape)
               for tensor in kv_caches.values()):
            raise ValueError("invalid paged KV cache shape")
        first = next(iter(kv_caches.values()))
        self._kv_caches = dict(kv_caches)
        self._load_stream = torch.cuda.Stream(device=first.device, priority=1)
        self._save_stream = torch.cuda.Stream(device=first.device, priority=1)

    def bind_gpu_block_pool(self, gpu_block_pool) -> None:
        self._gpu_block_pool = gpu_block_pool

    def bind_transport(self, transport) -> None:
        """Bind a sidecar object exposing lookup/get/put/release/abort."""
        self._transport = transport

    def _ids(self, blocks: Iterable[int]) -> tuple[int, ...]:
        values = tuple(int(x) for x in blocks)
        if len(set(values)) != len(values) or any(x < 0 for x in values):
            raise ValueError("invalid or aliased physical block IDs")
        return values

    def _next_generation(self, request_id: str) -> int:
        value = self._generation.get(request_id, 0) + 1
        self._generation[request_id] = value
        return value

    def get_num_new_matched_tokens(self, request, num_computed_tokens: int) -> tuple[int | None, bool]:
        tokens = tuple(int(x) for x in getattr(request, "prompt_token_ids", request))
        matched = 0
        with self._lock:
            for size in range(1, len(tokens) + 1):
                if tokens[:size] in self._entries:
                    matched = size
        request_id = str(getattr(request, "request_id", request))
        if self._transport is not None and hasattr(self._transport, "lookup"):
            try:
                result = self._transport.lookup(request_id, tokens)
                matched = int(result.get("hit_tokens", 0))
            except Exception:
                matched = 0
        block_size = int(getattr(getattr(self._vllm_config, "cache_config", None),
                                 "block_size", 16))
        matched -= matched % block_size
        if matched > num_computed_tokens:
            generation = self._next_generation(request_id)
            self._load_plans[request_id] = _Load(
                request_id, matched, (), generation, tokens[:matched]
            )
        return max(0, matched - num_computed_tokens), False

    def on_new_request(self, request) -> None:
        request_id = str(request.request_id)
        tokens = tuple(int(x) for x in request.prompt_token_ids)
        with self._lock:
            generation = self._next_generation(request_id)
            self._pending[request_id] = tokens
            self._load[request_id] = _Save(tokens, {}, generation)
            self._save_plans[request_id] = _Load(
                request_id, len(tokens), (), generation, tokens)

    def start_load_kv(self, forward_context, **kwargs) -> None:
        del forward_context, kwargs
        metadata = self._get_connector_metadata() if self.has_connector_metadata() else None
        plans = getattr(metadata, "loads", ()) if metadata is not None else ()
        for plan in plans:
            self._queue_load(plan)
        saves = getattr(metadata, "saves", ()) if metadata is not None else ()
        if len(saves) == 1:
            self._active_request.set(saves[0].request_id)
            self._load_plans[saves[0].request_id] = saves[0]
            with self._lock:
                self._pending[saves[0].request_id] = tuple(
                    int(x) for x in getattr(saves[0], "token_ids", ())
                )
                self._load[saves[0].request_id] = _Save(
                    self._pending[saves[0].request_id], {}, saves[0].generation
                )

    def wait_for_layer_load(self, layer_name: str) -> None:
        events = tuple(self._load_events.get(layer_name, ()))
        if not events:
            return
        import torch
        current = torch.cuda.current_stream()
        for event in events:
            current.wait_event(event)

    def _queue_load(self, plan) -> None:
        if self._transport is None or not self._kv_caches:
            return
        import torch
        request_id = str(plan.request_id)
        generation = plan.generation
        self._transfers[(request_id, generation)] = _Transfer(
            request_id, generation, "load", None, []
        )
        try:
            pages = self._transport.get_pages(plan, cancelled=lambda: self._cancelled(request_id, generation))
            with torch.cuda.stream(self._load_stream):
                events = {}
                for layer_name, destination in self._kv_caches.items():
                    source = pages[layer_name]
                    if source.device.type != "cpu":
                        source = source.cpu()
                    source = source.to(device=destination.device, dtype=destination.dtype, non_blocking=True)
                    ids = torch.as_tensor(plan.block_ids, device=destination.device, dtype=torch.long)
                    if source.shape[0] != ids.numel():
                        raise ValueError("source page count does not match destination blocks")
                    destination.index_copy_(0, ids, source)
                    event = torch.cuda.Event()
                    event.record(self._load_stream)
                    events[layer_name] = (event,)
            for layer_name, layer_events in events.items():
                self._load_events[layer_name] = layer_events
            self._transfers[(request_id, generation)].event = tuple(
                layer_events[0] for layer_events in events.values()
            )
        except Exception:
            self._load_errors.update(plan.block_ids)
            self._transfers.pop((request_id, generation), None)

    def _cancelled(self, request_id: str, generation: int) -> bool:
        transfer = self._transfers.get((request_id, generation))
        return transfer is not None and transfer.cancelled

    def save_kv_layer(self, layer_name, kv_layer, attn_metadata, **kwargs) -> None:
        del attn_metadata, kwargs
        request_id = self._active_request.get()
        if request_id is None and len(self._load) == 1:
            request_id = next(iter(self._load))
        self.save_kv_layer_for_request(request_id, layer_name, kv_layer)
        if request_id is None or self._save_stream is None:
            return
        save = self._load.get(request_id)
        if save is None or layer_name not in self._kv_caches:
            return
        import torch
        compute_stream = torch.cuda.current_stream(device=kv_layer.device)
        selected = kv_layer.index_select(
            0, torch.as_tensor(self._save_blocks(request_id), device=kv_layer.device)
        )
        staging = torch.empty_like(selected, device="cpu", pin_memory=True)
        with torch.cuda.stream(self._save_stream):
            done = torch.cuda.Event()
            done.record(compute_stream)
            self._save_stream.wait_event(done)
            staging.copy_(selected, non_blocking=True)
            copied = torch.cuda.Event()
            copied.record(self._save_stream)
        transfer_key = (request_id, save.generation)
        transfer = self._transfers.get(transfer_key)
        if transfer is None:
            self._transfers[transfer_key] = _Transfer(
                request_id, save.generation, "save", copied, [staging, selected],
            )
        else:
            transfer.event = copied
            transfer.buffers.extend((staging, selected))
        with self._lock:
            current = self._load.get(request_id)
            if current is save and isinstance(current.payload, dict):
                current.payload[layer_name] = staging

    def _save_blocks(self, request_id: str) -> tuple[int, ...]:
        plan = self._load_plans.get(request_id)
        return plan.block_ids if plan is not None else ()

    def save_kv_layer_for_request(self, request_id, layer_name, kv_layer) -> None:
        if request_id is None:
            return
        with self._lock:
            if request_id in self._pending:
                save = self._load.get(request_id)
                if save is not None:
                    if not isinstance(save.payload, dict):
                        save.payload = {}
                    save.payload[layer_name] = kv_layer

    def begin_save(self, request_id: str, token_ids) -> None:
        """Start a public save transaction before layer callbacks arrive."""
        with self._lock:
            self._active_request.set(request_id)
            tokens = tuple(int(x) for x in token_ids)
            self._pending[request_id] = tokens
            previous = self._load.get(request_id)
            generation = previous.generation + 1 if previous is not None else 1
            self._load[request_id] = _Save(
                tokens, {}, generation
            )
            self._load_plans[request_id] = _Load(request_id, len(tokens), (), generation)

    def wait_for_save(self) -> dict[str, Exception]:
        errors = {}
        attempted: set[str] = set()
        if self._put is None:
            return errors
        while True:
            with self._lock:
                pending = list(self._pending.items())
            candidate = next(
                ((request_id, key) for request_id, key in pending
                 if request_id not in self._saving and request_id not in attempted), None
            )
            if candidate is None:
                break
            request_id, key = candidate
            attempted.add(request_id)
            with self._lock:
                if request_id in self._saving:
                    continue
                save = self._load.get(request_id)
                if save is None:
                    continue
                self._saving.add(request_id)
            if save is not None:
                # Keep both maps until the store acknowledges success. A raised
                # callback therefore leaves an idempotent retryable transaction.
                try:
                    transfer = self._transfers.get((request_id, save.generation))
                    if transfer is not None:
                        events = transfer.event if isinstance(transfer.event, tuple) else (transfer.event,)
                        for event in events:
                            if event is not None:
                                event.synchronize()
                        if transfer.cancelled:
                            raise RuntimeError("save transfer cancelled")
                    self._put(save.key, save.payload)
                except Exception as error:
                    errors[request_id] = error
                    if self._transport is not None and hasattr(self._transport, "record_failure"):
                        self._transport.record_failure("publish", error)
                    with self._lock:
                        self._saving.discard(request_id)
                    continue
                with self._lock:
                    # Do not remove a replacement transaction published while
                    # the callback was outside the lock.
                    current = self._load.get(request_id)
                    if current is save and current.generation == save.generation:
                        self._load.pop(request_id, None)
                        self._pending.pop(request_id, None)
                    self._saving.discard(request_id)
        return errors

    def request_finished(self, request, block_ids) -> tuple[bool, dict[str, Any] | None]:
        request_blocks = self._ids(block_ids)
        request_id = str(getattr(request, "request_id", request))
        with self._lock:
            self._finished.add(request_id)
            if str(self._role).lower().endswith("scheduler") and request_id in self._pending:
                self._pending.pop(request_id, None)
                self._load.pop(request_id, None)
                self._load_plans.pop(request_id, None)
                self._save_plans.pop(request_id, None)
                return True, None
            raw_tokens = getattr(request, "prompt_token_ids", None)
            tokens = tuple(int(x) for x in raw_tokens) if raw_tokens is not None else self._pending.get(request_id, ())
            previous = self._load.get(request_id)
            generation = previous.generation if previous is not None else self._next_generation(request_id)
            payload = previous.payload if previous is not None else {}
            self._pending[request_id] = tokens
            self._load[request_id] = _Save(tokens, payload, generation)
            self._load_plans[request_id] = _Load(
                request_id, len(tokens), request_blocks, generation, tokens
            )
        return True, None

    def update_state_after_alloc(self, request, blocks, num_external_tokens: int):
        block_ids = blocks.get_block_ids() if hasattr(blocks, "get_block_ids") else blocks
        if block_ids and isinstance(block_ids[0], (list, tuple)):
            if len(block_ids) != 1:
                raise ValueError("KVStoreConnector supports one KV cache group")
            block_ids = block_ids[0]
        physical = self._ids(block_ids)
        request_id = str(getattr(request, "request_id", request))
        tokens = tuple(int(x) for x in getattr(request, "prompt_token_ids", request))
        with self._lock:
            save = self._save_plans.get(request_id)
            if save is not None:
                self._save_plans[request_id] = _Load(
                    request_id, len(tokens), physical, save.generation, tokens)
        if num_external_tokens > 0:
            with self._lock:
                self._pending[request_id] = tokens[:num_external_tokens]
                plan = self._load_plans.get(request_id)
                generation = plan.generation if plan is not None else self._next_generation(request_id)
                blocks_per_hit = max(1, (num_external_tokens + 15) // 16)
                self._load_plans[request_id] = _Load(
                    request_id, num_external_tokens, physical[:blocks_per_hit], generation,
                    tokens[:num_external_tokens]
                )

    @contextmanager
    def request_scope(self, request_id: str):
        token = self._active_request.set(request_id)
        try:
            yield
        finally:
            self._active_request.reset(token)

    def build_connector_meta(self, scheduler_output):
        del scheduler_output
        with self._lock:
            loads = tuple(self._load_plans.values())
            saves = tuple(
                _Load(request_id, len(tokens), self._save_plans[request_id].block_ids,
                      self._load[request_id].generation, tuple(tokens))
                for request_id, tokens in self._pending.items()
                if request_id in self._save_plans and request_id in self._load
            )
            self._load_plans.clear()
            self._save_plans.clear()
        return _Metadata(loads=loads, saves=saves)

    def build_connector_worker_meta(self):
        return None

    def update_connector_output(self, connector_output) -> None:
        del connector_output

    def get_finished(self, finished_req_ids: set[str]):
        completed = set()
        with self._lock:
            for request_id in finished_req_ids:
                candidates = [t for t in self._transfers.values() if t.request_id == request_id]
                if candidates and all(
                    all(event is None or event.query() for event in
                        (t.event if isinstance(t.event, tuple) else (t.event,)))
                    for t in candidates
                ):
                    completed.add(request_id)
                    for key in tuple(self._transfers):
                        if key[0] == request_id:
                            self._transfers.pop(key, None)
        return completed, None

    def get_block_ids_with_load_errors(self) -> set[int]:
        errors = set(self._load_errors)
        self._load_errors.clear()
        return errors

    def handle_preemptions(self, metadata) -> None:
        for plan in getattr(metadata, "loads", ()):
            self.cancel(str(plan.request_id))

    def cancel(self, request_id: str) -> None:
        for transfer in self._transfers.values():
            if transfer.request_id == request_id:
                transfer.cancelled = True
        self._load_events = {name: events for name, events in self._load_events.items()
                             if not name.startswith(request_id)}
        if self._transport is not None and hasattr(self._transport, "cancel"):
            self._transport.cancel(request_id)

    def shutdown(self) -> None:
        for transfer in self._transfers.values():
            events = transfer.event if isinstance(transfer.event, tuple) else (transfer.event,)
            for event in events:
                if event is not None:
                    event.synchronize()
        self._transfers.clear()
        self._load_events.clear()
        self._kv_caches.clear()
        if self._transport is not None and hasattr(self._transport, "close"):
            self._transport.close()

    def bind_store(self, put: Callable[[tuple[int, ...], Any], None]) -> None:
        with self._lock:
            self._put = put

    def publish(self, request_id: str, token_ids, payload) -> None:
        self.begin_save(request_id, token_ids)
        with self._lock:
            save = self._load[request_id]
            self._load[request_id] = _Save(save.key, payload, save.generation)
        self.wait_for_save()
