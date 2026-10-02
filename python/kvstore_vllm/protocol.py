"""Small protobuf-wire codec for ``kvstore_integration_v1.proto``.

This intentionally implements only protobuf wire types used by the frozen v1
schema, avoiding a runtime dependency on protoc or google.protobuf.
"""

from __future__ import annotations

from dataclasses import dataclass, field, fields
import hashlib
import struct


UNSPECIFIED, NEGOTIATE, LOOKUP, RESERVE, PUT, GET, RELEASE, ABORT, COMMIT, CANCEL = range(10)
OK, INVALID_ARGUMENT, NOT_FOUND, ALREADY_EXISTS, LIMIT_EXCEEDED, CORRUPTION, UNSUPPORTED, CANCELLED, DEADLINE_EXCEEDED, BUSY, INTERNAL, IO_ERROR = range(12)


@dataclass
class TraceContext:
    request_id: str = ""
    model_id: str = ""
    tenant_id: str = ""


@dataclass
class Capabilities:
    major: int = 0
    minor: int = 0
    pinned_cpu: bool = False
    cuda_ipc: bool = False


@dataclass
class TensorManifest:
    version: int = 0
    tenant_id: str = ""
    model_id: str = ""
    model_revision: str = ""
    adapter_id: str = ""
    adapter_revision: str = ""
    tokenizer_revision: str = ""
    cache_format: str = ""
    cache_format_version: int = 0
    token_digest: bytes = b""
    token_count: int = 0
    layer_begin: int = 0
    layer_count: int = 0
    dtype: int = 0
    shape: list[int] = field(default_factory=list)
    axis_order: list[int] = field(default_factory=list)
    strides_bytes: list[int] = field(default_factory=list)
    layout: int = 0
    key_value_packing: int = 0
    block_tokens: int = 0
    device_kind: int = 0
    device_index: int = 0
    tensor_parallel_rank: int = 0
    tensor_parallel_size: int = 0
    pipeline_parallel_rank: int = 0
    pipeline_parallel_size: int = 0
    payload_bytes: int = 0
    chunk_bytes: int = 0
    chunk_alignment_bytes: int = 0
    chunk_count: int = 0
    compression: int = 0
    payload_digest: bytes = b""
    created_at_ns: int = 0
    accessed_at_ns: int = 0


@dataclass
class TokenRange:
    begin: int = 0
    end: int = 0


@dataclass
class Request:
    operation: int = 0
    trace: TraceContext | None = None
    capabilities: Capabilities | None = None
    manifest: TensorManifest | None = None
    reservation_id: int = 0
    lease_id: int = 0
    chunk_index: int = 0
    payload: bytes = b""
    payload_checksum: int = 0
    token_ids: list[int] = field(default_factory=list)
    prefix_lengths: list[int] = field(default_factory=list)
    exact: bool = False
    deadline_unix_ms: int = 0
    cancelled: bool = False


@dataclass
class Response:
    operation: int = 0
    trace: TraceContext | None = None
    status: int = 0
    message: str = ""
    capabilities: Capabilities | None = None
    manifest: TensorManifest | None = None
    reservation_id: int = 0
    lease_id: int = 0
    hit_tokens: int = 0
    hit_ranges: list[TokenRange] = field(default_factory=list)
    recompute_ranges: list[TokenRange] = field(default_factory=list)
    chunk_indices: list[int] = field(default_factory=list)
    layer_begin: int = 0
    layer_count: int = 0
    payload: bytes = b""
    payload_checksum: int = 0
    recompute: bool = False
    disk_bytes: int = 0
    disk_hit: bool = False


_TRACE = {1: ("request_id", "s"), 2: ("model_id", "s"), 3: ("tenant_id", "s")}
_CAPABILITIES = {1: ("major", "v"), 2: ("minor", "v"), 3: ("pinned_cpu", "b"), 4: ("cuda_ipc", "b")}
_MANIFEST = {
    1: ("version", "v"), 2: ("tenant_id", "s"), 3: ("model_id", "s"),
    4: ("model_revision", "s"), 5: ("adapter_id", "s"), 6: ("adapter_revision", "s"),
    7: ("tokenizer_revision", "s"), 8: ("cache_format", "s"), 9: ("cache_format_version", "v"),
    10: ("token_digest", "y"), 11: ("token_count", "v"), 12: ("layer_begin", "v"),
    13: ("layer_count", "v"), 14: ("dtype", "v"), 15: ("shape", "p"),
    16: ("axis_order", "p"), 17: ("strides_bytes", "p"), 18: ("layout", "v"),
    19: ("key_value_packing", "v"), 20: ("block_tokens", "v"), 21: ("device_kind", "v"),
    22: ("device_index", "v"), 23: ("tensor_parallel_rank", "v"), 24: ("tensor_parallel_size", "v"),
    25: ("pipeline_parallel_rank", "v"), 26: ("pipeline_parallel_size", "v"),
    27: ("payload_bytes", "v"), 28: ("chunk_bytes", "v"), 29: ("chunk_alignment_bytes", "v"),
    30: ("chunk_count", "v"), 31: ("compression", "v"), 32: ("payload_digest", "y"),
    33: ("created_at_ns", "v"), 34: ("accessed_at_ns", "v"),
}
_RANGE = {1: ("begin", "v"), 2: ("end", "v")}
_REQUEST = {
    1: ("operation", "v"), 2: ("trace", (TraceContext, _TRACE)),
    3: ("capabilities", (Capabilities, _CAPABILITIES)), 4: ("manifest", (TensorManifest, _MANIFEST)),
    5: ("reservation_id", "v"), 6: ("lease_id", "v"), 7: ("chunk_index", "v"),
    8: ("payload", "y"), 9: ("payload_checksum", "v"), 10: ("token_ids", "p"),
    11: ("prefix_lengths", "p"), 12: ("exact", "b"), 13: ("deadline_unix_ms", "v"),
    14: ("cancelled", "b"),
}
_RESPONSE = {
    1: ("operation", "v"), 2: ("trace", (TraceContext, _TRACE)), 3: ("status", "v"),
    4: ("message", "s"), 5: ("capabilities", (Capabilities, _CAPABILITIES)),
    6: ("manifest", (TensorManifest, _MANIFEST)), 7: ("reservation_id", "v"),
    8: ("lease_id", "v"), 9: ("hit_tokens", "v"), 10: ("hit_ranges", (TokenRange, _RANGE, True)),
    11: ("recompute_ranges", (TokenRange, _RANGE, True)), 12: ("chunk_indices", "p"),
    13: ("layer_begin", "v"), 14: ("layer_count", "v"), 15: ("payload", "y"),
    16: ("payload_checksum", "v"), 17: ("recompute", "b"), 18: ("disk_bytes", "v"),
    19: ("disk_hit", "b"),
}


def _varint(value: int) -> bytes:
    if value < 0:
        raise ValueError("protobuf integers must be non-negative")
    output = bytearray()
    while value > 127:
        output.append((value & 127) | 128)
        value >>= 7
    output.append(value)
    return bytes(output)


def _read_varint(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    for shift in range(0, 70, 7):
        if offset >= len(data):
            raise ValueError("truncated protobuf varint")
        byte = data[offset]
        offset += 1
        value |= (byte & 127) << shift
        if byte < 128:
            return value, offset
    raise ValueError("oversized protobuf varint")


def _encode(message, schema) -> bytes:
    output = bytearray()
    for number, (name, kind) in schema.items():
        value = getattr(message, name)
        if value is None or value is False or value == 0 or value == "" or value == b"" or value == []:
            continue
        if kind in ("v", "b"):
            output += _varint(number << 3) + _varint(int(value))
            continue
        if kind == "p":
            body = b"".join(_varint(int(item)) for item in value)
        elif kind == "s":
            body = value.encode("utf-8")
        elif kind == "y":
            body = bytes(value)
        elif isinstance(kind, tuple):
            values = value if len(kind) == 3 else (value,)
            for item in values:
                body = _encode(item, kind[1])
                output += _varint((number << 3) | 2) + _varint(len(body)) + body
            continue
        else:  # pragma: no cover - schemas above are static
            raise AssertionError(kind)
        output += _varint((number << 3) | 2) + _varint(len(body)) + body
    return bytes(output)


def _decode(data: bytes, cls, schema):
    result = cls()
    offset = 0
    while offset < len(data):
        tag, offset = _read_varint(data, offset)
        number, wire = tag >> 3, tag & 7
        if wire == 0:
            raw, offset = _read_varint(data, offset)
        elif wire == 2:
            size, offset = _read_varint(data, offset)
            end = offset + size
            if end > len(data):
                raise ValueError("truncated protobuf field")
            raw, offset = data[offset:end], end
        elif wire == 1:
            offset += 8
            raw = None
        elif wire == 5:
            offset += 4
            raw = None
        else:
            raise ValueError("unsupported protobuf wire type")
        if offset > len(data):
            raise ValueError("truncated protobuf fixed-width field")
        if number not in schema:
            continue
        name, kind = schema[number]
        if kind in ("v", "b"):
            if wire != 0:
                raise ValueError("invalid protobuf scalar wire type")
            setattr(result, name, bool(raw) if kind == "b" else raw)
        elif kind == "p":
            values = []
            if wire == 0:
                values.append(raw)
            elif wire == 2:
                inner = 0
                while inner < len(raw):
                    value, inner = _read_varint(raw, inner)
                    values.append(value)
            else:
                raise ValueError("invalid packed field wire type")
            getattr(result, name).extend(values)
        elif kind in ("s", "y"):
            if wire != 2:
                raise ValueError("invalid protobuf bytes wire type")
            setattr(result, name, raw.decode("utf-8") if kind == "s" else raw)
        elif isinstance(kind, tuple):
            if wire != 2:
                raise ValueError("invalid protobuf message wire type")
            item = _decode(raw, kind[0], kind[1])
            if len(kind) == 3:
                getattr(result, name).append(item)
            else:
                setattr(result, name, item)
    return result


def encode_request(request: Request) -> bytes:
    return _encode(request, _REQUEST)


def decode_request(data: bytes) -> Request:
    return _decode(data, Request, _REQUEST)


def encode_response(response: Response) -> bytes:
    return _encode(response, _RESPONSE)


def decode_response(data: bytes) -> Response:
    return _decode(data, Response, _RESPONSE)


def token_digest(token_ids) -> bytes:
    digest = hashlib.sha256()
    for token in token_ids:
        if not 0 <= int(token) <= 0xffffffff:
            raise ValueError("token ID is outside uint32")
        digest.update(struct.pack(">I", int(token)))
    return digest.digest()


def payload_chunks(payload: bytes, chunk_bytes: int) -> tuple[bytes, ...]:
    if chunk_bytes <= 0:
        raise ValueError("chunk_bytes must be positive")
    return tuple(payload[offset:offset + chunk_bytes] for offset in range(0, len(payload), chunk_bytes))


def manifest_copy(manifest: TensorManifest, **changes) -> TensorManifest:
    values = {item.name: getattr(manifest, item.name) for item in fields(manifest)}
    values.update(changes)
    return TensorManifest(**values)
