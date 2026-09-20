"""SGLang 0.5.19 dynamic HiCacheStorage backend."""

from __future__ import annotations

import time
import threading
from dataclasses import dataclass

import torch

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


class KVStoreHiCacheStorage(HiCacheStorage):
    """Dynamic backend using bounded, cancellation-aware page transport."""

    def __init__(self, storage_config=None, factory_kwargs=None, lookup=None,
                 publish=None, release=None, abort=None):
        super().__init__()
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
        self._pages = {}
        self._lock = threading.RLock()
        self._closed = threading.Event()

    def _key(self, key):
        return f"{self._namespace}:{key}"

    def exists(self, key):
        if self._closed.is_set():
            return False
        with self._lock:
            return self._key(key) in self._pages

    def batch_exists(self, keys, extra_info=None):
        del extra_info
        count = 0
        for key in keys:
            if not self.exists(key):
                break
            count += 1
        return count

    def get(self, key, target_location=None, target_sizes=None):
        del target_sizes
        if self._closed.is_set():
            return None
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
        del extra_info
        indices = [int(index) for index in host_indices.tolist()]
        stride = getattr(self.mem_pool_host, "page_size", 1)
        if len(indices) < len(keys) * stride:
            return [False] * len(keys)
        pool = self.mem_pool_host
        result = []
        for i, key in enumerate(keys):
            index = indices[i * stride]
            page = self.get(key)
            result.append(page is not None)
            if page is not None:
                pool.set_from_flat_data_page(index, page)
        return result

    def batch_set_v1(self, keys, host_indices, extra_info=None):
        del extra_info
        indices = [int(index) for index in host_indices.tolist()]
        pool = self.mem_pool_host
        stride = getattr(pool, "page_size", 1)
        if len(indices) < len(keys) * stride:
            return [False] * len(keys)
        return [self.set(key, pool.get_data_page(indices[i * stride]))
                for i, key in enumerate(keys)]

    def register_mem_pool_host(self, mem_pool_host):
        super().register_mem_pool_host(mem_pool_host)

    def register_mem_host_pool_v2(self, host_pool, host_pool_name):
        super().register_mem_host_pool_v2(host_pool, host_pool_name)

    def batch_exists_v2(self, keys, pool_transfers=None, extra_info=None):
        del pool_transfers, extra_info
        return PoolTransferResult(kv_hit_pages=self.batch_exists(keys),
                                  extra_pool_hit_pages={})

    def batch_get_v2(self, transfers, extra_info=None):
        del extra_info
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
                page = self.get(key)
                flags.append(page is not None)
                if page is not None:
                    pool.set_from_flat_data_page(indices[i * stride], page)
            result[transfer.name] = flags
        return result

    def batch_set_v2(self, transfers, extra_info=None):
        del extra_info
        result = {}
        for transfer in transfers:
            keys = transfer.keys or []
            pool = self.registered_pools[transfer.name]
            indices = transfer.host_indices.tolist() if transfer.host_indices is not None else []
            stride = getattr(pool, "page_size", 1)
            if len(indices) < len(keys) * stride:
                result[transfer.name] = [False] * len(keys)
            else:
                result[transfer.name] = [
                    self.set(key, pool.get_data_page(indices[i * stride]))
                    for i, key in enumerate(keys)
                ]
        return result

    def close(self):
        self._closed.set()

    def clear(self):
        with self._lock:
            self._pages.clear()

    def lookup(self, token_ids, *, exact=False, deadline=None, cancelled=False):
        if cancelled:
            return {"status": "cancelled", "hit_tokens": 0}
        if deadline is not None and time.monotonic() >= deadline:
            return {"status": "deadline_exceeded", "hit_tokens": 0}
        if self._lookup is None:
            return {"status": "not_found", "hit_tokens": 0}
        result = self._lookup(tuple(token_ids), exact, deadline)
        if deadline is not None and time.monotonic() >= deadline:
            lease_id = result.get("lease_id") if isinstance(result, dict) else None
            if lease_id is not None:
                self._release(lease_id)
            return {"status": "deadline_exceeded", "hit_tokens": 0}
        return result

    def publish(self, manifest, chunks):
        if self._publish is None:
            raise RuntimeError("sidecar publication transport is not configured")
        try:
            return self._publish(manifest, tuple(chunks))
        except BaseException:
            if self._abort is not None:
                self._abort(manifest.get("reservation_id", 0))
            raise

    def release(self, lease_id):
        if self._release is None:
            return None
        return self._release(lease_id)

    def abort(self, reservation_id):
        if self._abort is None:
            return None
        return self._abort(reservation_id)
