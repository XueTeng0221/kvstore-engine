"""Versioned P8.1 UDS framing transport.

The caller supplies protobuf Request bytes and receives protobuf Response bytes;
protobuf message construction stays in the framework adapter and this module
only owns bounded framing, reconnect, and socket lifecycle.
"""

from __future__ import annotations

import socket
import struct


class SessionTransport:
    def __init__(self, path: str, timeout: float = 5.0, max_frame: int = 16 * 1024 * 1024):
        self.path = path
        self.timeout = timeout
        self.max_frame = max_frame
        self._socket: socket.socket | None = None

    def connect(self) -> None:
        if "\x00" in self.path:
            raise ValueError("UDS path contains NUL")
        if self._socket is not None:
            return
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.settimeout(self.timeout)
            sock.connect(self.path)
        except BaseException:
            sock.close()
            raise
        self._socket = sock

    def exchange(self, request: bytes, operation: int | None = None) -> bytes:
        if len(request) == 0 or len(request) + 13 > self.max_frame:
            raise ValueError("protobuf frame exceeds configured limit")
        self.connect()
        try:
            if operation is None:
                operation = request[1] if len(request) > 1 and request[0] == 8 else 0
            if operation == 0:
                raise ValueError("protobuf operation is required")
            envelope = (b"KVP\x01" + bytes((operation, 0, 1, 0, 0)) +
                        struct.pack(">I", len(request)) + request)
            self._send(struct.pack("<I", len(envelope)) + envelope)
            header = self._read_exact(4)
            size = struct.unpack("<I", header)[0]
            if size == 0 or size > self.max_frame:
                raise ValueError("invalid response frame size")
            envelope = self._read_exact(size)
            if len(envelope) < 13 or envelope[:4] != b"KVP\x01":
                raise ValueError("invalid P8.1 response envelope")
            if envelope[4] != (operation | 128) or envelope[5:9] != b"\x00\x01\x00\x00":
                raise ValueError("invalid response operation or protocol version")
            payload_size = struct.unpack(">I", envelope[9:13])[0]
            if payload_size != len(envelope) - 13:
                raise ValueError("invalid P8.1 payload size")
            return envelope[13:]
        except (OSError, TimeoutError, ValueError):
            self.close()
            raise

    def close(self) -> None:
        if self._socket is not None:
            self._socket.close()
            self._socket = None

    def _send(self, data: bytes) -> None:
        assert self._socket is not None
        self._socket.sendall(data)

    def _read_exact(self, size: int) -> bytes:
        assert self._socket is not None
        result = bytearray()
        while len(result) < size:
            chunk = self._socket.recv(size - len(result))
            if not chunk:
                raise ConnectionError("UDS bridge closed")
            result.extend(chunk)
        return bytes(result)
