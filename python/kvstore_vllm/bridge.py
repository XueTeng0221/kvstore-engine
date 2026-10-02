"""Production P8.1 Session bridge for the vLLM 0.29.0 connector."""

from __future__ import annotations

import hashlib
import json
import math
import os
import threading
from dataclasses import dataclass

from . import protocol as pb
from .uds import Session, SessionError


@dataclass(frozen=True)
class _Geometry:
    layer_names: tuple[str, ...]
    block_tokens: int
    num_kv_heads: int
    head_size: int
    dtype: int
    element_bytes: int


def _contiguous_strides(shape: list[int], element_bytes: int) -> list[int]:
    strides = [0] * len(shape)
    stride = element_bytes
    for index in range(len(shape) - 1, -1, -1):
        strides[index] = stride
        stride *= shape[index]
    return strides


class VllmSessionBridge:
    """Maps vLLM block tensors to the canonical P8.1 block-major manifest."""

    def __init__(self, session: Session, geometry: _Geometry, config: dict):
        self._session = session
        self._geometry = geometry
        self._model_revision = str(config.get("model_revision", "unknown"))
        self._tokenizer_revision = str(config.get("tokenizer_revision", "unknown"))
        self._chunk_bytes = int(config.get("chunk_bytes", 1024 * 1024))
        self._audit_path = config.get("audit_path")
        if self._chunk_bytes <= 0 or self._chunk_bytes > 16 * 1024 * 1024:
            raise ValueError("chunk_bytes must be in [1, 16777216]")
        self._chunk_bytes -= self._chunk_bytes % 64
        if self._chunk_bytes == 0:
            raise ValueError("chunk_bytes must permit 64-byte alignment")
        self._cancel: dict[str, threading.Event] = {}
        self._lock = threading.Lock()

    @property
    def geometry(self) -> _Geometry:
        return self._geometry

    def configure_runtime_geometry(self, layer_names, tensor_shape) -> None:
        """Replace model-spec guesses with the worker's allocated page layout."""
        shape = tuple(int(value) for value in tensor_shape)
        if len(shape) == 5 and shape[1] == 2:
            block_tokens, num_kv_heads, head_size = shape[2:]
        elif len(shape) == 4 and shape[1] == 2 and shape[0] != 2:
            block_tokens = shape[2]
            if self._geometry.num_kv_heads <= 0 or self._geometry.head_size <= 0:
                raise ValueError("flattened runtime KV layout needs configured head geometry")
            num_kv_heads, head_size = self._geometry.num_kv_heads, self._geometry.head_size
            if shape[3] != num_kv_heads * head_size:
                raise ValueError("flattened runtime KV width does not match head geometry")
        elif len(shape) == 4 and shape[0] == 2 and shape[1] != 2:
            block_tokens = shape[2]
            if self._geometry.num_kv_heads <= 0 or self._geometry.head_size <= 0:
                raise ValueError("flattened runtime KV layout needs configured head geometry")
            num_kv_heads, head_size = self._geometry.num_kv_heads, self._geometry.head_size
            if shape[3] != num_kv_heads * head_size:
                raise ValueError("flattened runtime KV width does not match head geometry")
        else:
            raise ValueError(f"ambiguous or unsupported runtime KV cache shape: {shape}")
        self._geometry = _Geometry(
            tuple(str(name) for name in layer_names), block_tokens,
            num_kv_heads, head_size, self._geometry.dtype,
            self._geometry.element_bytes)

    def _record(self, operation: str, **values) -> None:
        if not self._audit_path:
            return
        values.setdefault("network_bytes", self._session.transport.network_bytes)
        record = json.dumps({"operation": operation, **values}, separators=(",", ":"))
        descriptor = os.open(self._audit_path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        try:
            os.write(descriptor, (record + "\n").encode())
        finally:
            os.close(descriptor)

    @classmethod
    def from_vllm_config(cls, vllm_config, kv_cache_config, extra: dict):
        path = extra.get("uds_path")
        if not path:
            raise ValueError("kv_connector_extra_config.uds_path is required")
        groups = tuple(getattr(kv_cache_config, "transfer_groups", ()) or
                       getattr(kv_cache_config, "kv_cache_groups", ()))
        if len(groups) != 1:
            raise ValueError("KVStoreConnector supports one transferable KV cache group")
        group = groups[0]
        spec = group.kv_cache_spec
        dtype_name = str(spec.dtype).replace("torch.", "")
        dtype_table = {"float16": (1, 2), "bfloat16": (2, 2), "float32": (3, 4)}
        if dtype_name not in dtype_table:
            raise ValueError(f"unsupported KV cache dtype: {spec.dtype}")
        dtype, element_bytes = dtype_table[dtype_name]
        geometry = _Geometry(
            tuple(group.layer_names), int(spec.block_size), int(spec.num_kv_heads),
            int(spec.head_size), dtype, element_bytes)
        model = getattr(vllm_config, "model_config", None)
        tenant = str(extra.get("tenant_id", "default"))
        model_id = str(extra.get("model_id", getattr(model, "served_model_name", "model")))
        resolved = dict(extra)
        resolved.setdefault("model_revision", getattr(model, "revision", None) or "unknown")
        resolved.setdefault("tokenizer_revision",
                            getattr(model, "tokenizer_revision", None) or "unknown")
        timeout = float(extra.get("timeout_seconds", 5.0))
        return cls(Session(str(path), tenant_id=tenant, model_id=model_id, timeout=timeout),
                   geometry, resolved)

    def _event(self, request_id: str) -> threading.Event:
        with self._lock:
            return self._cancel.setdefault(request_id, threading.Event())

    def _manifest(self, tokens, payload_digest=b"") -> pb.TensorManifest:
        tokens = tuple(int(token) for token in tokens)
        blocks = math.ceil(len(tokens) / self._geometry.block_tokens)
        shape = [len(self._geometry.layer_names), 2, blocks,
                 self._geometry.block_tokens, self._geometry.num_kv_heads,
                 self._geometry.head_size]
        payload_bytes = math.prod(shape) * self._geometry.element_bytes
        return pb.TensorManifest(
            version=1, tenant_id=self._session.tenant_id, model_id=self._session.model_id,
            model_revision=self._model_revision,
            tokenizer_revision=self._tokenizer_revision,
            cache_format="vllm-0.29.0-kvconnector-v1", cache_format_version=1,
            token_digest=pb.token_digest(tokens), token_count=len(tokens),
            layer_count=len(self._geometry.layer_names), dtype=self._geometry.dtype,
            shape=shape, axis_order=[2, 1, 4, 3, 5, 6],
            strides_bytes=_contiguous_strides(shape, self._geometry.element_bytes),
            layout=2, key_value_packing=1, block_tokens=self._geometry.block_tokens,
            device_kind=1, tensor_parallel_size=1, pipeline_parallel_size=1,
            payload_bytes=payload_bytes, chunk_bytes=self._chunk_bytes,
            chunk_alignment_bytes=64,
            chunk_count=math.ceil(payload_bytes / self._chunk_bytes),
            payload_digest=payload_digest)

    def lookup(self, request_id: str, tokens) -> dict:
        if len(tokens) < self._geometry.block_tokens:
            return {"hit_tokens": 0}
        event = self._event(request_id)
        event.clear()
        manifest = self._manifest(tokens)
        prefixes = range(self._geometry.block_tokens, len(tokens) + 1,
                         self._geometry.block_tokens)
        try:
            response = self._session.lookup(manifest, tokens, exact=False,
                                            prefix_lengths=prefixes, cancel=event)
        except SessionError:
            self._record("lookup", request_id=request_id, hit_tokens=0,
                         disk_hit=False, disk_bytes=0)
            return {"hit_tokens": 0}
        finally:
            with self._lock:
                self._cancel.pop(request_id, None)
        try:
            self._record("lookup", request_id=request_id, hit_tokens=response.hit_tokens,
                         disk_hit=response.disk_hit, disk_bytes=response.disk_bytes)
            return {"hit_tokens": response.hit_tokens}
        finally:
            if response.lease_id:
                self._session.release(response.lease_id)

    def get_pages(self, plan, cancelled):
        import torch

        request_id = str(plan.request_id)
        event = self._event(request_id)
        if cancelled():
            event.set()
        manifest = self._manifest(plan.token_ids)
        try:
            response = self._session.lookup(manifest, plan.token_ids, exact=False,
                                            prefix_lengths=range(
                                                self._geometry.block_tokens,
                                                len(plan.token_ids) + 1,
                                                self._geometry.block_tokens),
                                            cancel=event)
            payload = self._session.get_chunks(response, cancel=event)
            self._record("get_pages", request_id=request_id,
                         hit_tokens=response.hit_tokens, payload_bytes=len(payload),
                         disk_hit=response.disk_hit, disk_bytes=response.disk_bytes)
            if len(payload) != manifest.payload_bytes:
                raise ValueError("P8.1 payload size does not match vLLM manifest")
            shape = tuple(manifest.shape)
            value = torch.frombuffer(bytearray(payload), dtype={
                1: torch.float16, 2: torch.bfloat16, 3: torch.float32,
            }[manifest.dtype]).reshape(shape)
            return {name: value[index].clone().pin_memory()
                    for index, name in enumerate(self._geometry.layer_names)}
        except Exception as error:
            self._record("get_pages_error", request_id=request_id, error=str(error))
            raise
        finally:
            with self._lock:
                self._cancel.pop(request_id, None)

    def publish(self, tokens, pages) -> None:
        import torch

        if set(pages) != set(self._geometry.layer_names):
            raise ValueError("publication does not contain every configured KV layer")
        ordered = []
        for name in self._geometry.layer_names:
            tensor = pages[name]
            if not isinstance(tensor, torch.Tensor):
                raise ValueError("publication layer is not a tensor")
            expected = (2, math.ceil(len(tokens) / self._geometry.block_tokens),
                        self._geometry.block_tokens, self._geometry.num_kv_heads,
                        self._geometry.head_size)
            if tuple(tensor.shape) != expected:
                raise ValueError(
                    f"publication tensor geometry for {name} is {tuple(tensor.shape)}, "
                    f"expected {expected}")
            ordered.append(tensor.detach().cpu().contiguous())
        canonical = torch.stack(ordered).contiguous()
        payload = canonical.view(torch.uint8).numpy().tobytes()
        manifest = self._manifest(tokens, hashlib.sha256(payload).digest())
        if len(payload) != manifest.payload_bytes:
            raise ValueError("publication tensor geometry does not match token count")
        try:
            response = self._session.publish(
                manifest, pb.payload_chunks(payload, manifest.chunk_bytes))
        except SessionError as error:
            if error.response.status == pb.ALREADY_EXISTS:
                self._record("publish_duplicate", token_count=len(tokens),
                             payload_bytes=len(payload))
                return
            raise
        self._record("publish", token_count=len(tokens), payload_bytes=len(payload))
        if response.lease_id:
            self._session.release(response.lease_id)

    def cancel(self, request_id: str) -> None:
        with self._lock:
            event = self._cancel.get(request_id)
        if event is not None:
            event.set()

    def record_failure(self, operation: str, error: Exception) -> None:
        self._record(operation + "_error", error=str(error))

    def close(self) -> None:
        self._session.close()
