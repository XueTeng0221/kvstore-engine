"""Versioned P8.1 UDS framing transport.

The caller supplies protobuf Request bytes and receives protobuf Response bytes;
protobuf message construction stays in the framework adapter and this module
only owns bounded framing, reconnect, and socket lifecycle.
"""

from __future__ import annotations

import socket
import struct
import threading
import time
import zlib

from . import protocol as pb


class SessionTransport:
    def __init__(self, path: str, timeout: float = 5.0, max_frame: int = 16 * 1024 * 1024):
        self.path = path
        self.timeout = timeout
        self.max_frame = max_frame
        self._socket: socket.socket | None = None
        self._state_lock = threading.Lock()
        self._exchange_lock = threading.Lock()
        self._cancelled = threading.Event()
        self._generation = 0

    @property
    def generation(self) -> int:
        with self._state_lock:
            return self._generation

    def connect(self) -> None:
        if "\x00" in self.path:
            raise ValueError("UDS path contains NUL")
        with self._state_lock:
            if self._socket is not None:
                return
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.settimeout(min(self.timeout, 0.1))
            sock.connect(self.path)
        except BaseException:
            sock.close()
            raise
        with self._state_lock:
            if self._cancelled.is_set():
                sock.close()
                raise InterruptedError("transport cancelled")
            self._socket = sock
            self._generation += 1

    def exchange(self, request: bytes, operation: int | None = None, *,
                 cancel: threading.Event | None = None,
                 deadline: float | None = None) -> bytes:
        if len(request) == 0 or len(request) + 13 > self.max_frame:
            raise ValueError("protobuf frame exceeds configured limit")
        limit = time.monotonic() + self.timeout if deadline is None else deadline
        with self._exchange_lock:
            self._cancelled.clear()
            self.connect()
            try:
                self._check(cancel, limit)
                if operation is None:
                    operation = request[1] if len(request) > 1 and request[0] == 8 else 0
                if operation == 0:
                    raise ValueError("protobuf operation is required")
                envelope = (b"KVP\x01" + bytes((operation, 0, 1, 0, 0)) +
                            struct.pack(">I", len(request)) + request)
                self._send(struct.pack("<I", len(envelope)) + envelope, cancel, limit)
                header = self._read_exact(4, cancel, limit)
                size = struct.unpack("<I", header)[0]
                if size == 0 or size > self.max_frame:
                    raise ValueError("invalid response frame size")
                envelope = self._read_exact(size, cancel, limit)
                if len(envelope) < 13 or envelope[:4] != b"KVP\x01":
                    raise ValueError("invalid P8.1 response envelope")
                if envelope[4] != (operation | 128) or envelope[5:9] != b"\x00\x01\x00\x00":
                    raise ValueError("invalid response operation or protocol version")
                payload_size = struct.unpack(">I", envelope[9:13])[0]
                if payload_size != len(envelope) - 13:
                    raise ValueError("invalid P8.1 payload size")
                return envelope[13:]
            except (OSError, TimeoutError, ValueError, InterruptedError):
                self.close()
                raise

    def cancel(self, request_id: str | None = None) -> None:
        """Interrupt a blocked exchange; in-flight CUDA DMA remains event-owned."""
        del request_id
        self._cancelled.set()
        self.close()

    def close(self) -> None:
        with self._state_lock:
            sock, self._socket = self._socket, None
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()

    def _check(self, cancel, deadline) -> None:
        if self._cancelled.is_set() or (cancel is not None and cancel.is_set()):
            raise InterruptedError("transport cancelled")
        if time.monotonic() >= deadline:
            raise TimeoutError("transport deadline exceeded")

    def _send(self, data: bytes, cancel, deadline) -> None:
        offset = 0
        while offset < len(data):
            self._check(cancel, deadline)
            with self._state_lock:
                sock = self._socket
            if sock is None:
                raise InterruptedError("transport closed")
            try:
                offset += sock.send(data[offset:])
            except socket.timeout:
                continue

    def _read_exact(self, size: int, cancel, deadline) -> bytes:
        result = bytearray()
        while len(result) < size:
            self._check(cancel, deadline)
            with self._state_lock:
                sock = self._socket
            if sock is None:
                raise InterruptedError("transport closed")
            try:
                chunk = sock.recv(size - len(result))
            except socket.timeout:
                continue
            if not chunk:
                raise ConnectionError("UDS bridge closed")
            result.extend(chunk)
        return bytes(result)


class SessionError(RuntimeError):
    """A P8.1 response reported a non-OK status."""

    def __init__(self, response: pb.Response):
        self.response = response
        super().__init__(response.message or f"integration status {response.status}")


class Session:
    """Typed P8.1 session over :class:`SessionTransport`."""

    def __init__(self, path: str | None = None, *, tenant_id: str, model_id: str,
                 timeout: float = 5.0, transport: SessionTransport | None = None):
        if transport is None:
            if path is None:
                raise ValueError("path or transport is required")
            transport = SessionTransport(path, timeout=timeout)
        self.transport = transport
        self.tenant_id = tenant_id
        self.model_id = model_id
        self._sequence = 0
        self._negotiated_generation = -1
        self._lock = threading.RLock()

    def close(self) -> None:
        self.transport.close()
        self._negotiated_generation = -1

    def cancel(self, request_id: str | None = None) -> None:
        self.transport.cancel(request_id)
        self._negotiated_generation = -1

    def _trace(self) -> pb.TraceContext:
        self._sequence += 1
        return pb.TraceContext(
            request_id=f"python-{self._sequence}", model_id=self.model_id,
            tenant_id=self.tenant_id)

    def _exchange(self, request: pb.Request, *, cancel=None, deadline=None,
                  check=True) -> pb.Response:
        request.trace = request.trace or self._trace()
        response = pb.decode_response(self.transport.exchange(
            pb.encode_request(request), request.operation, cancel=cancel, deadline=deadline))
        if response.operation != request.operation:
            raise ValueError("response operation mismatch")
        if check and response.status != pb.OK:
            raise SessionError(response)
        return response

    def negotiate(self, *, force=False) -> pb.Response:
        with self._lock:
            self.transport.connect()
            if not force and self._negotiated_generation == self.transport.generation:
                return pb.Response(operation=pb.NEGOTIATE, status=pb.OK)
            response = self._exchange(pb.Request(
                operation=pb.NEGOTIATE,
                capabilities=pb.Capabilities(major=1, pinned_cpu=True)))
            self._negotiated_generation = self.transport.generation
            return response

    def _call(self, request: pb.Request, *, cancel=None, deadline=None,
              retry=False) -> pb.Response:
        with self._lock:
            for attempt in range(2 if retry else 1):
                try:
                    self.negotiate()
                    return self._exchange(request, cancel=cancel, deadline=deadline)
                except (TimeoutError, InterruptedError):
                    self._negotiated_generation = -1
                    raise
                except (ConnectionError, BrokenPipeError, OSError):
                    self._negotiated_generation = -1
                    if attempt or not retry:
                        raise
            raise AssertionError("unreachable")

    def lookup(self, manifest: pb.TensorManifest, token_ids, *, exact=False,
               prefix_lengths=(), cancel=None, deadline=None) -> pb.Response:
        deadline_ms = 0
        if deadline is not None:
            remaining = max(0.0, deadline - time.monotonic())
            deadline_ms = int((time.time() + remaining) * 1000)
        return self._call(pb.Request(
            operation=pb.LOOKUP, manifest=manifest,
            token_ids=[int(value) for value in token_ids],
            prefix_lengths=[int(value) for value in prefix_lengths], exact=exact,
            deadline_unix_ms=deadline_ms), cancel=cancel, deadline=deadline, retry=True)

    def get(self, lease_id: int, chunk_index: int = 0, *, cancel=None,
            deadline=None) -> bytes:
        response = self._call(pb.Request(
            operation=pb.GET, lease_id=lease_id, chunk_index=chunk_index),
            cancel=cancel, deadline=deadline, retry=False)
        if zlib.crc32(response.payload) & 0xffffffff != response.payload_checksum:
            raise ValueError("GET payload checksum mismatch")
        return response.payload

    def release(self, lease_id: int) -> pb.Response:
        return self._call(pb.Request(operation=pb.RELEASE, lease_id=lease_id))

    def reserve(self, manifest: pb.TensorManifest) -> int:
        return self._call(pb.Request(operation=pb.RESERVE, manifest=manifest)).reservation_id

    def put(self, reservation_id: int, chunk_index: int, payload: bytes) -> pb.Response:
        return self._call(pb.Request(
            operation=pb.PUT, reservation_id=reservation_id, chunk_index=chunk_index,
            payload=payload, payload_checksum=zlib.crc32(payload) & 0xffffffff))

    def commit(self, reservation_id: int) -> pb.Response:
        return self._call(pb.Request(operation=pb.COMMIT, reservation_id=reservation_id))

    def abort(self, reservation_id: int) -> pb.Response:
        return self._call(pb.Request(operation=pb.ABORT, reservation_id=reservation_id))

    def publish(self, manifest: pb.TensorManifest, chunks) -> pb.Response:
        reservation_id = self.reserve(manifest)
        try:
            for index, chunk in enumerate(chunks):
                self.put(reservation_id, index, bytes(chunk))
            return self.commit(reservation_id)
        except BaseException:
            try:
                self.abort(reservation_id)
            except BaseException:
                pass
            raise

    def get_chunks(self, response: pb.Response, *, cancel=None,
                   deadline=None) -> bytes:
        try:
            return b"".join(self.get(response.lease_id, index, cancel=cancel,
                                     deadline=deadline)
                            for index in response.chunk_indices)
        finally:
            if response.lease_id:
                try:
                    self.release(response.lease_id)
                except (OSError, SessionError):
                    pass
