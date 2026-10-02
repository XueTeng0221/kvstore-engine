"""Live C++ UdsBridge interoperability; run through CTest or its environment."""

import os
import selectors
import socket
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[2] / "python"))
sys.path.insert(0, os.environ.get("KVSTORE_PYTHON_PROTO_DIR", str(Path(__file__).parents[2] / "build" / "python")))
from kvstore_vllm.uds import Session, SessionError, SessionTransport
from kvstore_vllm import protocol as integration
try:
    import kvstore_integration_v1_pb2 as pb
    _PROTOBUF_IMPORT_ERROR = ""
except ModuleNotFoundError as exc:
    if "KVSTORE_UDS_FIXTURE" in os.environ:
        raise RuntimeError("CTest live UDS requires generated bindings and google.protobuf") from exc
    pb = None
    _PROTOBUF_IMPORT_ERROR = f"protobuf test dependency unavailable: {exc}"


@unittest.skipUnless(
    "KVSTORE_UDS_FIXTURE" in os.environ,
    "live UDS fixture environment is provided by CTest; " + _PROTOBUF_IMPORT_ERROR,
)
class LiveUdsBridgeTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.path = str(Path(temporary.name) / "bridge.sock")
        self.proc = self.spawn(self.path)
        with selectors.DefaultSelector() as selector:
            selector.register(self.proc.stdout, selectors.EVENT_READ)
            self.assertTrue(selector.select(timeout=5), "fixture readiness timed out")
            self.assertEqual(os.read(self.proc.stdout.fileno(), 6), b"READY\n")

    def spawn(self, path, mode=None):
        proc = subprocess.Popen(
            [os.environ["KVSTORE_UDS_FIXTURE"], path, "tenant", "test-model"] + ([mode] if mode else []),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.cleanup_process, proc)
        return proc

    @staticmethod
    def cleanup_process(proc):
        try:
            if proc.poll() is None:
                proc.kill()
            proc.wait(timeout=5)
        finally:
            for stream in (proc.stdin, proc.stdout, proc.stderr):
                stream.close()

    def transport(self):
        transport = SessionTransport(self.path, timeout=2)
        self.addCleanup(transport.close)
        return transport

    def client(self):
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(client.close)
        client.settimeout(2)
        client.connect(self.path)
        return client

    @staticmethod
    def request(operation, request_id="request"):
        req = pb.Request(operation=operation)
        req.trace.request_id = request_id
        req.trace.tenant_id = "tenant"
        req.trace.model_id = "test-model"
        if operation == pb.NEGOTIATE:
            req.capabilities.major = 1
            req.capabilities.pinned_cpu = True
        return req

    def exchange(self, transport, req, status=None):
        if status is None:
            status = pb.OK
        response = pb.Response.FromString(
            transport.exchange(req.SerializeToString(), operation=req.operation))
        self.assertEqual(response.operation, req.operation)
        self.assertEqual(response.trace, req.trace)
        self.assertEqual(response.status, status, response.message)
        return response

    def negotiate(self, transport, request_id="negotiate"):
        response = self.exchange(transport, self.request(pb.NEGOTIATE, request_id))
        self.assertEqual(response.capabilities.major, 1)
        self.assertTrue(response.capabilities.pinned_cpu)
        self.assertFalse(response.capabilities.cuda_ipc)

    def test_two_simultaneous_clients_and_invalid_requests(self):
        self.assertEqual(os.stat(self.path).st_mode & 0o777, 0o600)
        first, second = self.transport(), self.transport()
        self.negotiate(first, "first")
        # The first connection stays open and idle while the second exchanges.
        self.negotiate(second, "second")
        for transport in (first, second):
            self.exchange(transport, self.request(pb.NEGOTIATE), pb.ALREADY_EXISTS)
            self.exchange(transport, self.request(pb.RESERVE), pb.INVALID_ARGUMENT)
            self.exchange(transport, self.request(pb.LOOKUP), pb.INVALID_ARGUMENT)
            req = self.request(pb.GET)
            req.lease_id = 999999
            self.exchange(transport, req, pb.NOT_FOUND)
        req = self.request(pb.RESERVE)
        req.trace.tenant_id = "wrong-tenant"
        self.exchange(first, req, pb.INVALID_ARGUMENT)
        first.close()
        self.negotiate(first, "reconnected")

    def test_python_session_publication_lookup_and_reconnect(self):
        """Exercise the real C++ Session, not a Python fake server."""
        session = Session(self.path, tenant_id="tenant", model_id="test-model", timeout=2)
        self.addCleanup(session.close)
        payload = b"x" * 128
        description = integration.TensorManifest(
            version=1, tenant_id="tenant", model_id="test-model",
            model_revision="rev", tokenizer_revision="tok",
            cache_format="live", cache_format_version=1,
            token_digest=integration.token_digest((1, 2, 3, 4)), token_count=4,
            layer_count=1, dtype=1, shape=[2, 1, 4, 2, 4],
            axis_order=[1, 2, 3, 5, 6], strides_bytes=[64, 64, 16, 8, 2],
            layout=1, key_value_packing=1, block_tokens=0, device_kind=1,
            tensor_parallel_size=1, pipeline_parallel_size=1,
            payload_bytes=len(payload), chunk_bytes=128,
            chunk_alignment_bytes=64, chunk_count=1,
            payload_digest=__import__("hashlib").sha256(payload).digest(),
        )
        with self.assertRaises(SessionError) as missed:
            session.lookup(description, (1, 2, 3, 4), exact=True)
        self.assertEqual(missed.exception.response.status, integration.NOT_FOUND)
        committed = session.publish(description, (payload,))
        self.assertTrue(committed.lease_id)
        session.release(committed.lease_id)
        hit = session.lookup(description, (1, 2, 3, 4), exact=True)
        self.assertEqual(hit.hit_tokens, 4)
        self.assertEqual(session.get_chunks(hit), payload)
        session.close()
        reconnected = session.lookup(description, (1, 2, 3, 4), exact=True)
        self.assertEqual(reconnected.hit_tokens, 4)
        session.release(reconnected.lease_id)

    @staticmethod
    def frame(req):
        payload = req.SerializeToString()
        envelope = b"KVP\x01" + bytes((req.operation, 0, 1, 0, 0))
        envelope += struct.pack(">I", len(payload)) + payload
        return struct.pack("<I", len(envelope)) + envelope

    @staticmethod
    def read_exact(client, size):
        result = bytearray()
        while len(result) < size:
            part = client.recv(size - len(result))
            if not part:
                raise AssertionError("unexpected bridge EOF")
            result.extend(part)
        return bytes(result)

    def read_response(self, client, req, status):
        size = struct.unpack("<I", self.read_exact(client, 4))[0]
        self.assertGreaterEqual(size, 13)
        self.assertLessEqual(size, 16 * 1024 * 1024)
        envelope = self.read_exact(client, size)
        self.assertEqual(envelope[:9], b"KVP\x01" + bytes((req.operation | 128, 0, 1, 0, 0)))
        self.assertEqual(struct.unpack(">I", envelope[9:13])[0], size - 13)
        response = pb.Response.FromString(envelope[13:])
        self.assertEqual(response.operation, req.operation)
        self.assertEqual(response.trace, req.trace)
        self.assertEqual(response.status, status)

    def test_fragmented_and_coalesced_frames(self):
        client = self.client()
        negotiate = self.request(pb.NEGOTIATE)
        for byte in self.frame(negotiate):
            client.sendall(bytes((byte,)))
        self.read_response(client, negotiate, pb.OK)
        reserve = self.request(pb.RESERVE, "reserve")
        get = self.request(pb.GET, "get")
        client.sendall(self.frame(reserve) + self.frame(get))
        self.read_response(client, reserve, pb.INVALID_ARGUMENT)
        self.read_response(client, get, pb.NOT_FOUND)

    def test_malformed_frames_close_only_offending_connection(self):
        valid = self.frame(self.request(pb.NEGOTIATE))
        bad_magic = valid[:4] + b"BAD!" + valid[8:]
        bad_operation = valid[:8] + bytes((pb.GET,)) + valid[9:]
        for frame in (b"\0" * 4, struct.pack("<I", 0xffffffff), bad_magic,
                      bad_operation, struct.pack("<I", 14) + b"KVP\x01\x01\0\x01\0\0\0\0\0\x01\xff"):
            with self.subTest(frame=frame):
                client = self.client()
                client.sendall(frame)
                try:
                    self.assertEqual(client.recv(1), b"")
                except ConnectionResetError:
                    pass
                client.close()
        self.negotiate(self.transport())

    def test_stop_with_idle_and_partial_clients_is_bounded(self):
        idle = self.transport()
        partial = self.client()
        self.negotiate(idle)
        partial.sendall(b"\x20\0")
        self.proc.stdin.write(b"stop\n")
        self.proc.stdin.flush()
        self.assertEqual(self.proc.wait(timeout=3), 0)
        self.assertFalse(os.path.lexists(self.path))
        self.assertEqual(partial.recv(1), b"")

    def test_path_conflict_preserves_socket_and_regular_file(self):
        inode = os.stat(self.path).st_ino
        conflict = self.spawn(self.path)
        out, err = conflict.communicate(timeout=3)
        self.assertNotEqual(conflict.returncode, 0)
        self.assertNotIn(b"READY", out)
        self.assertTrue(err)
        self.assertEqual(os.stat(self.path).st_ino, inode)
        self.negotiate(self.transport())
        path = Path(self.path).with_name("existing-file")
        path.write_bytes(b"preserve me")
        conflict = self.spawn(str(path))
        conflict.communicate(timeout=3)
        self.assertNotEqual(conflict.returncode, 0)
        self.assertEqual(path.read_bytes(), b"preserve me")

    def test_stop_preserves_replacement_socket(self):
        os.unlink(self.path)
        replacement = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(replacement.close)
        replacement.bind(self.path)
        replacement_inode = os.stat(self.path).st_ino
        self.proc.stdin.write(b"stop\n")
        self.proc.stdin.flush()
        self.assertEqual(self.proc.wait(timeout=3), 0)
        self.assertTrue(os.path.lexists(self.path))
        self.assertEqual(os.stat(self.path).st_ino, replacement_inode)

    def test_sequential_connections_recycle_worker_slots(self):
        for number in range(96):
            transport = SessionTransport(self.path, timeout=2)
            try:
                self.negotiate(transport, f"recycle-{number}")
            finally:
                transport.close()

    def test_concurrent_start_stop_does_not_leave_path(self):
        path = self.path + ".concurrent"
        proc = self.spawn(path, "concurrent")
        out, err = proc.communicate(timeout=20)
        self.assertEqual(proc.returncode, 0, err.decode())
        self.assertEqual(out, b"CONCURRENT 32\n")
        self.assertFalse(os.path.lexists(path))

    def test_rejects_non_sticky_world_writable_parent(self):
        parent = Path(self.path).with_name("unsafe-parent")
        parent.mkdir()
        parent.chmod(0o777)
        self.addCleanup(lambda: parent.chmod(0o700))
        process = self.spawn(str(parent / "bridge.sock"), "unsafe-parent")
        out, err = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, err.decode())
        self.assertEqual(out, b"UNSAFE PARENT REJECTED\n")

    def test_nul_and_failed_start_preserve_existing_path(self):
        existing = Path(self.path)
        inode = os.stat(existing).st_ino
        invalid = SessionTransport(self.path + "\x00bad")
        self.addCleanup(invalid.close)
        self.assertRaises(ValueError, invalid.connect)
        invalid_cpp = self.spawn(self.path, "nul")
        out, err = invalid_cpp.communicate(timeout=5)
        self.assertEqual(invalid_cpp.returncode, 0, err.decode())
        self.assertEqual(out, b"NUL REJECTED\n")
        conflict = self.spawn(self.path)
        conflict.communicate(timeout=3)
        self.assertEqual(os.stat(existing).st_ino, inode)


if __name__ == "__main__":
    unittest.main()
