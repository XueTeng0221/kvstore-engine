"""Minimal vLLM KVConnectorBase_V1 bridge.

The scheduler and worker callbacks share a bounded in-process prefix index. The
transport hook is deliberately injected so production deployments can connect
the existing P8.1 Session without making framework code depend on C++ symbols.
"""

from __future__ import annotations

from dataclasses import dataclass
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


@dataclass
class _Entry:
    token_count: int
    payload: Any

@dataclass
class _Save:
    key: tuple[int, ...]
    payload: Any
    generation: int


class KVStoreConnector(KVConnectorBase_V1):
    """Framework callback implementation with exact and prefix lookup."""

    def __init__(self, vllm_config, role, kv_cache_config):
        super().__init__(vllm_config, role, kv_cache_config)
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

    def get_num_new_matched_tokens(self, request, num_computed_tokens: int) -> tuple[int | None, bool]:
        tokens = tuple(int(x) for x in getattr(request, "prompt_token_ids", request))
        matched = 0
        with self._lock:
            for size in range(1, len(tokens) + 1):
                if tokens[:size] in self._entries:
                    matched = size
        return max(0, matched - num_computed_tokens), False

    def start_load_kv(self, forward_context, **kwargs) -> None:
        del forward_context, kwargs

    def wait_for_layer_load(self, layer_name: str) -> None:
        del layer_name

    def save_kv_layer(self, layer_name, kv_layer, attn_metadata, **kwargs) -> None:
        del attn_metadata, kwargs
        request_id = self._active_request.get()
        self.save_kv_layer_for_request(request_id, layer_name, kv_layer)

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
            self._pending[request_id] = tuple(int(x) for x in token_ids)
            previous = self._load.get(request_id)
            generation = previous.generation + 1 if previous is not None else 1
            self._load[request_id] = _Save(
                tuple(int(x) for x in token_ids), {}, generation
            )

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
                    self._put(save.key, save.payload)
                except Exception as error:
                    errors[request_id] = error
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
        del block_ids
        request_id = str(getattr(request, "request_id", request))
        with self._lock:
            self._finished.add(request_id)
        return False, None

    def update_state_after_alloc(self, request, blocks, num_external_tokens: int):
        del blocks
        if num_external_tokens > 0:
            request_id = str(getattr(request, "request_id", request))
            tokens = tuple(int(x) for x in getattr(request, "prompt_token_ids", request))
            with self._lock:
                self._pending[request_id] = tokens[:num_external_tokens]

    @contextmanager
    def request_scope(self, request_id: str):
        token = self._active_request.set(request_id)
        try:
            yield
        finally:
            self._active_request.reset(token)

    def build_connector_meta(self, scheduler_output):
        del scheduler_output
        return None

    def bind_store(self, put: Callable[[tuple[int, ...], Any], None]) -> None:
        with self._lock:
            self._put = put

    def publish(self, request_id: str, token_ids, payload) -> None:
        self.begin_save(request_id, token_ids)
        with self._lock:
            save = self._load[request_id]
            self._load[request_id] = _Save(save.key, payload, save.generation)
        self.wait_for_save()
