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


class SessionTransport:
    def __init__(self, path: str, timeout: float = 5.0, max_frame: int = 16 * 1024 * 1024):
        self.path = path
        self.timeout = timeout
        self.max_frame = max_frame
        self._socket: socket.socket | None = None
        self._state_lock = threading.Lock()
        self._exchange_lock = threading.Lock()
        self._cancelled = threading.Event()

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
