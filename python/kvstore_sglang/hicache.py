"""SGLang dynamic HiCacheStorage-compatible prefix adapter."""

from __future__ import annotations

import time


class KVStoreHiCacheStorage:
    """Small runtime adapter with the interface_v1 operation names.

    The callable transport is injected by the deployment sidecar and keeps
    request cancellation and timeout decisions at the framework boundary.
    """

    def __init__(self, lookup, publish, release, abort):
        self._lookup = lookup
        self._publish = publish
        self._release = release
        self._abort = abort

    def lookup(self, token_ids, *, exact=False, deadline=None, cancelled=False):
        if cancelled:
            return {"status": "cancelled", "hit_tokens": 0}
        if deadline is not None and time.monotonic() >= deadline:
            return {"status": "deadline_exceeded", "hit_tokens": 0}
        result = self._lookup(tuple(token_ids), exact, deadline)
        if deadline is not None and time.monotonic() >= deadline:
            lease_id = result.get("lease_id") if isinstance(result, dict) else None
            if lease_id is not None:
                self._release(lease_id)
            return {"status": "deadline_exceeded", "hit_tokens": 0}
        return result

    def publish(self, manifest, chunks):
        try:
            return self._publish(manifest, tuple(chunks))
        except BaseException:
            self._abort(manifest.get("reservation_id", 0))
            raise

    def release(self, lease_id):
        return self._release(lease_id)

    def abort(self, reservation_id):
        return self._abort(reservation_id)
