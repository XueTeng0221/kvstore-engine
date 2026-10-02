"""SGLang 0.5.19 dynamic HiCacheStorage backend."""

from __future__ import annotations

import time
import threading
import hashlib
import math
import struct
import json
import os
import contextvars
from dataclasses import dataclass

import torch

from kvstore_vllm import protocol as integration
from kvstore_vllm.uds import Session, SessionError

try:
    from sglang.srt.mem_cache.hicache_storage import (
        HiCacheStorage,
        PoolTransferResult,
    )
except ImportError as exc:  # pragma: no cover - environment validation
    class HiCacheStorage:
        """Import-only fallback; SGLang validates the real subclass at launch."""

        def __init__(self):
            self.registered_pools = {}

        def register_mem_pool_host(self, mem_pool_host):
            self.mem_pool_host = mem_pool_host

        def register_mem_host_pool_v2(self, host_pool, host_pool_name):
            self.registered_pools[host_pool_name] = host_pool

    @dataclass
    class PoolTransferResult:
        kv_hit_pages: int
        extra_pool_hit_pages: dict


class _CallableEvent:
    def __init__(self, callback):
        self._callback = callback

    def is_set(self):
        return bool(self._callback())


_stock_cancellation = contextvars.ContextVar("kvstore_sglang_stock_cancellation", default=None)
_stock_bridge_installed = False
_stock_bridge_lock = threading.Lock()


def _install_stock_cancellation_bridge():
    """Preserve stock SGLang's termination predicate in ExtraInfo.

    SGLang 0.5.19 creates ``HiCacheStorageExtraInfo`` without a cancellation
    field.  Its controller does, however, expose the request-owned
    ``PrefetchOperation.is_terminated`` predicate.  The small context bridge
    keeps that predicate attached to the exact ExtraInfo object passed to the
    backend, including the worker thread where the storage call runs.
    """
    global _stock_bridge_installed
    with _stock_bridge_lock:
        if _stock_bridge_installed:
            return
        _install_stock_cancellation_bridge_locked()


def _install_stock_cancellation_bridge_locked():
    global _stock_bridge_installed
    try:
        import sglang.srt.managers.cache_controller as cache_controller
        import sglang.srt.mem_cache.hybrid_cache.hybrid_cache_controller as hybrid_controller
    except ImportError:
        return
    original_info = cache_controller.HiCacheStorageExtraInfo

    class BoundExtraInfo(original_info):
        def __init__(self, prefix_keys=None, extra_info=None):
            values = dict(extra_info or {})
            terminated = _stock_cancellation.get()
            if terminated is not None:
                values.setdefault("cancelled", terminated)
                values.setdefault("cancel_event", _CallableEvent(terminated))
            super().__init__(prefix_keys=prefix_keys, extra_info=values or None)

    cache_controller.HiCacheStorageExtraInfo = BoundExtraInfo
    hybrid_controller.HiCacheStorageExtraInfo = BoundExtraInfo

    def wrap(method):
        def guarded(self, operation, *args, **kwargs):
            token = _stock_cancellation.set(operation.is_terminated)
            try:
                return method(self, operation, *args, **kwargs)
            finally:
                _stock_cancellation.reset(token)
        return guarded

    cache_controller.HiCacheController._page_transfer = wrap(
        cache_controller.HiCacheController._page_transfer)
    cache_controller.HiCacheController._storage_hit_query = wrap(
        cache_controller.HiCacheController._storage_hit_query)
    if hasattr(hybrid_controller.HybridCacheController, "_page_transfer"):
        hybrid_controller.HybridCacheController._page_transfer = wrap(
            hybrid_controller.HybridCacheController._page_transfer)
    if hasattr(hybrid_controller.HybridCacheController, "_storage_hit_query"):
        hybrid_controller.HybridCacheController._storage_hit_query = wrap(
            hybrid_controller.HybridCacheController._storage_hit_query)
    _stock_bridge_installed = True


class KVStoreHiCacheStorage(HiCacheStorage):
    """Dynamic backend using bounded, cancellation-aware page transport."""

    def __init__(self, storage_config=None, factory_kwargs=None, lookup=None,
                 publish=None, release=None, abort=None):
        super().__init__()
        _install_stock_cancellation_bridge()
        # Preserve the callable fixture API while supporting SGLang's dynamic
        # constructor `(HiCacheStorageConfig, factory_kwargs)`.
        if callable(storage_config) and callable(factory_kwargs):
            lookup, publish, release, abort = storage_config, factory_kwargs, lookup, publish
            storage_config = None
        extra = getattr(storage_config, "extra_config", {}) or {}
        self._lookup = lookup
        self._publish = publish
        self._release = release
        self._abort = abort
        self._timeout = float(extra.get("timeout_seconds", 5.0))
        self._namespace = str(extra.get("namespace", "default"))
        self._audit_path = extra.get("audit_path")
        uds_path = extra.get("uds_path")
        self._session = None
        if uds_path:
            tenant = str(extra.get("tenant_id", self._namespace))
            model = str(extra.get("model_id", getattr(storage_config, "model_name", "model")))
            self._session = Session(str(uds_path), tenant_id=tenant, model_id=model,
                                    timeout=self._timeout)
            self._manifests = {}
            self._tensor_meta = {}
            self._model_revision = str(extra.get("model_revision", "unknown"))
            self._tokenizer_revision = str(extra.get("tokenizer_revision", "unknown"))
            self._chunk_bytes = int(extra.get("chunk_bytes", 1024 * 1024))
            if self._chunk_bytes <= 0 or self._chunk_bytes % 64:
                raise ValueError("chunk_bytes must be a positive multiple of 64")
        else:
            self._pages = {}
        self._lock = threading.RLock()
        self._closed = threading.Event()

    def _record(self, operation, **values):
        if not self._audit_path:
            return
        try:
            if self._session is not None:
                values.setdefault("network_bytes", self._session.transport.network_bytes)
            record = json.dumps({"operation": operation, **values}, separators=(",", ":"))
            descriptor = os.open(self._audit_path,
                                 os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW,
                                 0o600)
            try:
                data = (record + "\n").encode()
                offset = 0
                while offset < len(data):
                    try:
                        written = os.write(descriptor, data[offset:])
                    except InterruptedError:
                        continue
                    if written <= 0:
                        return
                    offset += written
            finally:
                os.close(descriptor)
        except Exception:
            return

    def _key(self, key):
        return f"{self._namespace}:{key}"

    def _tokens(self, key):
        digest = hashlib.sha256(self._key(key).encode("utf-8")).digest()
        return (struct.unpack(">I", digest[:4])[0],)

    def _manifest(self, key, tensor=None, payload=None):
        cached = self._manifests.get(self._key(key))
        if tensor is None:
            if cached is None:
                return None
            return integration.manifest_copy(cached, payload_digest=b"")
        tensor = tensor.detach()
        dtype = {torch.float16: 1, torch.bfloat16: 2, torch.float32: 3}.get(tensor.dtype)
        if dtype is None:
            raise ValueError("UDS storage supports float16, bfloat16, and float32 tensors")
        element_bytes = tensor.element_size()
        payload_bytes = (tensor.numel() * element_bytes if payload is None else len(payload))
        if payload_bytes == 0 or payload_bytes % (2 * element_bytes):
            raise ValueError("tensor payload does not fit the P8 key/value axis")
        width = payload_bytes // (2 * element_bytes)
        strides = [element_bytes * width, element_bytes * width,
                   element_bytes * width, element_bytes * width, element_bytes]
        tokens = self._tokens(key)
        chunk_bytes = min(self._chunk_bytes, max(64, math.ceil(payload_bytes / 64) * 64))
        manifest = integration.TensorManifest(
            version=1, tenant_id=self._session.tenant_id, model_id=self._session.model_id,
            model_revision=self._model_revision, tokenizer_revision=self._tokenizer_revision,
            cache_format="sglang-hicache-interface_v1", cache_format_version=1,
            token_digest=integration.token_digest(tokens), token_count=1,
            layer_count=1, dtype=dtype, shape=[2, 1, 1, 1, width],
            axis_order=[1, 2, 3, 5, 6], strides_bytes=strides,
            layout=1, key_value_packing=1, device_kind=1,
            tensor_parallel_size=1, pipeline_parallel_size=1,
            payload_bytes=payload_bytes, chunk_bytes=chunk_bytes,
            chunk_alignment_bytes=64, chunk_count=math.ceil(payload_bytes / chunk_bytes),
            payload_digest=b"" if payload is None else hashlib.sha256(payload).digest())
        self._manifests[self._key(key)] = manifest
        self._tensor_meta[self._key(key)] = (tensor.dtype, tuple(tensor.shape))
        return manifest

    def _ensure_manifest(self, key, pool=None):
        manifest = self._manifest(key)
        if manifest is not None or self._session is None:
            return manifest
        if pool is None:
            pool = getattr(self, "mem_pool_host", None)
        if pool is None:
            pool = next(iter(getattr(self, "registered_pools", {}).values()), None)
        if pool is None or not hasattr(pool, "get_dummy_flat_data_page"):
            return None
        return self._manifest(key, pool.get_dummy_flat_data_page())

    @staticmethod
    def _operation_context(extra_info):
        values = getattr(extra_info, "extra_info", None) or {}
        cancel = values.get("cancel_event")
        cancelled = values.get("cancelled")
        if cancel is None and callable(cancelled):
            cancel = _CallableEvent(cancelled)
        deadline = values.get("deadline")
        return cancel, deadline

    @staticmethod
    def _check_operation(cancel, deadline):
        if cancel is not None and cancel.is_set():
            raise InterruptedError("SGLang HiCache operation cancelled")
        if deadline is not None and time.monotonic() >= deadline:
            raise TimeoutError("SGLang HiCache operation deadline exceeded")

    @staticmethod
    def _tensor_bytes(tensor):
        value = tensor.detach().cpu().contiguous()
        return value.view(torch.uint8).numpy().tobytes()

    def _remote_lookup(self, key, *, pool=None, cancel=None, deadline=None):
        cancelled = cancel is not None and cancel.is_set()
        if cancelled:
            return None
        manifest = self._ensure_manifest(key, pool)
        if manifest is None:
            return None
        try:
            response = self._session.lookup(manifest, self._tokens(key), exact=True,
                                            cancel=cancel, deadline=deadline)
            self._record("lookup", hit_tokens=response.hit_tokens,
                         disk_hit=response.disk_hit, disk_bytes=response.disk_bytes)
            return response
        except SessionError as error:
            if error.response.status in (integration.NOT_FOUND, integration.DEADLINE_EXCEEDED,
                                         integration.CANCELLED):
                return None
            raise

    def exists(self, key):
        if self._closed.is_set():
            return False
        if self._session is not None:
            response = self._remote_lookup(key)
            if response is None:
                return False
            self._session.release(response.lease_id)
            return response.hit_tokens > 0
        with self._lock:
            return self._key(key) in self._pages

    def batch_exists(self, keys, extra_info=None):
        cancel, deadline = self._operation_context(extra_info)
        count = 0
        for key in keys:
            self._check_operation(cancel, deadline)
            if self._session is not None:
                response = self._remote_lookup(key, cancel=cancel, deadline=deadline)
                exists = response is not None and response.hit_tokens > 0
                if response is not None and response.lease_id:
                    self._session.release(response.lease_id)
            else:
                exists = self.exists(key)
            if not exists:
                break
            count += 1
        return count

    def get(self, key, target_location=None, target_sizes=None):
        del target_sizes
        if self._closed.is_set():
            return None
        if self._session is not None:
            # After a backend restart the manifest index is intentionally empty.
            # SGLang supplies the destination page here, so rebuild the query
            # identity from its shape/dtype instead of consulting local state.
            if self._manifest(key) is None and target_location is not None:
                self._manifest(key, target_location)
            response = self._remote_lookup(key)
            if response is None:
                return None
            payload = self._session.get_chunks(response)
            self._record("get_pages", hit_tokens=response.hit_tokens,
                         payload_bytes=len(payload), disk_hit=response.disk_hit,
                         disk_bytes=response.disk_bytes)
            metadata = self._tensor_meta.get(self._key(key))
            if metadata is None:
                metadata = (target_location.dtype, tuple(target_location.shape))
            dtype, shape = metadata
            value = torch.frombuffer(bytearray(payload), dtype=dtype).reshape(shape).clone()
            if target_location is not None:
                target_location.copy_(value)
                return target_location
            return value
        with self._lock:
            value = self._pages.get(self._key(key))
            value = None if value is None else value.clone()
        if value is not None and target_location is not None:
            target_location.copy_(value)
            return target_location
        return value

    def batch_get(self, keys, target_locations=None, target_sizes=None):
        del target_sizes
        targets = target_locations if target_locations is not None else [None] * len(keys)
        return [self.get(key, target) for key, target in zip(keys, targets)]

    def set(self, key, value=None, target_location=None, target_sizes=None):
        del target_sizes
        source = value if value is not None else target_location
        if self._closed.is_set() or source is None:
            return False
        if not isinstance(source, torch.Tensor):
            source = torch.as_tensor(source)
        if self._session is not None:
            payload = self._tensor_bytes(source)
            manifest = self._manifest(key, source, payload)
            chunks = integration.payload_chunks(payload, manifest.chunk_bytes)
            response = self._session.publish(manifest, chunks)
            self._record("publish", payload_bytes=len(payload))
            if response.lease_id:
                self._session.release(response.lease_id)
            return True
        with self._lock:
            self._pages[self._key(key)] = source.detach().cpu().clone()
        return True

    def batch_set(self, keys, values=None, target_locations=None, target_sizes=None):
        del target_sizes
        sources = values if values is not None else target_locations
        if sources is None or len(sources) != len(keys):
            return False
        return all(self.set(key, value) for key, value in zip(keys, sources))

    def batch_get_v1(self, keys, host_indices, extra_info=None):
        cancel, deadline = self._operation_context(extra_info)
        indices = [int(index) for index in host_indices.tolist()]
        stride = getattr(self.mem_pool_host, "page_size", 1)
        if len(indices) < len(keys) * stride:
            return [False] * len(keys)
        pool = self.mem_pool_host
        result = []
        for i, key in enumerate(keys):
            if cancel is not None and cancel.is_set():
                result.extend([False] * (len(keys) - i))
                break
            self._check_operation(cancel, deadline)
            index = indices[i * stride]
            if self._session is not None and self._manifest(key) is None:
                target = pool.get_data_page(index)
                self._manifest(key, target)
            response = (self._remote_lookup(key, pool=pool, cancel=cancel, deadline=deadline)
                        if self._session is not None else None)
            page = None
            if response is not None:
                payload = self._session.get_chunks(response, cancel=cancel, deadline=deadline)
                self._record("get_pages", hit_tokens=response.hit_tokens,
                             payload_bytes=len(payload))
                target = pool.get_data_page(index)
                page = torch.frombuffer(bytearray(payload), dtype=target.dtype).reshape(target.shape).clone()
            elif self._session is None:
                page = self.get(key)
            result.append(page is not None)
            if page is not None:
                pool.set_from_flat_data_page(index, page)
        return result

    def batch_set_v1(self, keys, host_indices, extra_info=None):
        cancel, deadline = self._operation_context(extra_info)
        indices = [int(index) for index in host_indices.tolist()]
        pool = self.mem_pool_host
        stride = getattr(pool, "page_size", 1)
        if len(indices) < len(keys) * stride:
            return [False] * len(keys)
        result = []
        for i, key in enumerate(keys):
            if cancel is not None and cancel.is_set():
                result.extend([False] * (len(keys) - i))
                break
            self._check_operation(cancel, deadline)
            result.append(self.set(key, pool.get_data_page(indices[i * stride])))
        return result

    def register_mem_pool_host(self, mem_pool_host):
        super().register_mem_pool_host(mem_pool_host)

    def register_mem_host_pool_v2(self, host_pool, host_pool_name):
        super().register_mem_host_pool_v2(host_pool, host_pool_name)

    def batch_exists_v2(self, keys, pool_transfers=None, extra_info=None):
        cancel, deadline = self._operation_context(extra_info)
        result = PoolTransferResult(kv_hit_pages=self.batch_exists(keys, extra_info),
                                    extra_pool_hit_pages={})
        for transfer in pool_transfers or ():
            transfer_keys = transfer.keys or []
            self._check_operation(cancel, deadline)
            hit = self.batch_exists(transfer_keys, extra_info)
            policy = getattr(transfer, "hit_policy", "all_pages")
            policy_value = getattr(policy, "value", policy)
            if policy_value == "trailing_pages":
                # Stock SGLang passes only the trailing sidecar page list here;
                # a complete sidecar suffix preserves the whole KV prefix.
                missing = len(transfer_keys) - hit
                hit = max(0, result.kv_hit_pages - missing)
            elif policy_value != "all_pages":
                raise ValueError(f"unsupported SGLang pool hit policy: {policy_value}")
            result.extra_pool_hit_pages[str(transfer.name)] = hit
        return result

    def batch_get_v2(self, transfers, extra_info=None):
        cancel, deadline = self._operation_context(extra_info)
        result = {}
        for transfer in transfers:
            keys = transfer.keys or []
            pool = self.registered_pools[transfer.name]
            flags = []
            indices = transfer.host_indices.tolist() if transfer.host_indices is not None else []
            stride = getattr(pool, "page_size", 1)
            if len(indices) < len(keys) * stride:
                result[transfer.name] = [False] * len(keys)
                continue
            for i, key in enumerate(keys):
                if cancel is not None and cancel.is_set():
                    flags.extend([False] * (len(keys) - i))
                    break
                self._check_operation(cancel, deadline)
                if self._session is not None and self._manifest(key) is None:
                    target = pool.get_data_page(indices[i * stride])
                    self._manifest(key, target)
                response = (self._remote_lookup(key, pool=pool, cancel=cancel,
                                                deadline=deadline)
                            if self._session is not None else None)
                page = None
                if response is not None:
                    payload = self._session.get_chunks(response, cancel=cancel,
                                                       deadline=deadline)
                    self._record("get_pages", hit_tokens=response.hit_tokens,
                                 payload_bytes=len(payload), disk_hit=response.disk_hit,
                                 disk_bytes=response.disk_bytes)
                    target = pool.get_data_page(indices[i * stride])
                    page = torch.frombuffer(bytearray(payload), dtype=target.dtype).reshape(target.shape).clone()
                elif self._session is None:
                    page = self.get(key)
                flags.append(page is not None)
                if page is not None:
                    pool.set_from_flat_data_page(indices[i * stride], page)
            result[transfer.name] = flags
        return result

    def batch_set_v2(self, transfers, extra_info=None):
        cancel, deadline = self._operation_context(extra_info)
        result = {}
        for transfer in transfers:
            keys = transfer.keys or []
            pool = self.registered_pools[transfer.name]
            indices = transfer.host_indices.tolist() if transfer.host_indices is not None else []
            stride = getattr(pool, "page_size", 1)
            if len(indices) < len(keys) * stride:
                result[transfer.name] = [False] * len(keys)
            else:
                flags = []
                for i, key in enumerate(keys):
                    if cancel is not None and cancel.is_set():
                        flags.extend([False] * (len(keys) - i))
                        break
                    self._check_operation(cancel, deadline)
                    flags.append(self.set(key, pool.get_data_page(indices[i * stride])))
                result[transfer.name] = flags
        return result

    def close(self):
        self._closed.set()
        if self._session is not None:
            self._session.close()

    def clear(self):
        with self._lock:
            if self._session is None:
                self._pages.clear()
            else:
                self._manifests.clear()
                self._tensor_meta.clear()

    def lookup(self, token_ids, *, exact=False, deadline=None, cancelled=False):
        if cancelled:
            return {"status": "cancelled", "hit_tokens": 0}
        if deadline is not None and time.monotonic() >= deadline:
            return {"status": "deadline_exceeded", "hit_tokens": 0}
        if self._lookup is None:
            if self._session is None:
                return {"status": "not_found", "hit_tokens": 0}
            # Public token-prefix lookup requires the caller's canonical manifest;
            # page-oriented HiCache calls use exists/get above.
            return {"status": "not_found", "hit_tokens": 0}
        result = self._lookup(tuple(token_ids), exact, deadline)
        self._record("lookup", hit_tokens=(result.get("hit_tokens", 0)
                                            if isinstance(result, dict) else 0))
        if deadline is not None and time.monotonic() >= deadline:
            lease_id = result.get("lease_id") if isinstance(result, dict) else None
            if lease_id is not None:
                self._release(lease_id)
            return {"status": "deadline_exceeded", "hit_tokens": 0}
        return result

    def publish(self, manifest, chunks):
        if self._publish is None:
            if self._session is None:
                raise RuntimeError("sidecar publication transport is not configured")
            if isinstance(manifest, dict):
                manifest = integration.TensorManifest(**manifest)
            response = self._session.publish(manifest, chunks)
            self._record("publish", payload_bytes=manifest.payload_bytes)
            return response
        try:
            result = self._publish(manifest, tuple(chunks))
            self._record("publish")
            return result
        except BaseException:
            if self._abort is not None:
                self._abort(manifest.get("reservation_id", 0))
            raise

    def release(self, lease_id):
        if self._release is None:
            return None if self._session is None else self._session.release(lease_id)
        return self._release(lease_id)

    def abort(self, reservation_id):
        if self._abort is None:
            return None if self._session is None else self._session.abort(reservation_id)
        return self._abort(reservation_id)
