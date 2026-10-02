import os
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest
import zlib
from pathlib import Path
from types import SimpleNamespace

import torch

sys.path.insert(0, str(Path(__file__).parents[2] / "python"))

from kvstore_vllm import protocol as pb
from kvstore_vllm.uds import Session, SessionError


class FakeUdsServer:
    def __init__(self, path, state=None):
        self.path = path
        self.state = state if state is not None else {}
        self.delay_operation = None
        self.partial = False
        self.operations = []
        self._stop = threading.Event()
        self._clients = []
        self._listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._listener.settimeout(0.05)
        self._listener.bind(path)
        self._listener.listen()
        self._thread = threading.Thread(target=self._serve)
        self._thread.start()

    @staticmethod
    def _read_exact(client, size):
        output = bytearray()
        while len(output) < size:
            chunk = client.recv(size - len(output))
            if not chunk:
                raise EOFError
            output.extend(chunk)
        return bytes(output)

    def _serve(self):
        workers = []
        while not self._stop.is_set():
            try:
                client, _ = self._listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self._clients.append(client)
            worker = threading.Thread(target=self._client, args=(client,))
            workers.append(worker)
            worker.start()
        for worker in workers:
            worker.join(timeout=1)

    def _client(self, client):
        negotiated = False
        reservation = None
        chunks = {}
        try:
            with client:
                while not self._stop.is_set():
                    size = struct.unpack("<I", self._read_exact(client, 4))[0]
                    envelope = self._read_exact(client, size)
                    request = pb.decode_request(envelope[13:])
                    self.operations.append(request.operation)
                    if self.delay_operation == request.operation:
                        time.sleep(0.3)
                    response = pb.Response(operation=request.operation, trace=request.trace)
                    if request.operation == pb.NEGOTIATE:
                        negotiated = True
                        response.capabilities = pb.Capabilities(major=1, pinned_cpu=True)
                    elif not negotiated:
                        response.status = pb.UNSUPPORTED
                    elif request.operation == pb.RESERVE:
                        reservation = request.manifest
                        chunks = {}
                        response.reservation_id = 41
                    elif request.operation == pb.PUT:
                        chunks[request.chunk_index] = request.payload
                    elif request.operation == pb.COMMIT:
                        payload = b"".join(chunks[index] for index in sorted(chunks))
                        self.state["entry"] = (reservation, payload)
                        response.lease_id = 71
                    elif request.operation == pb.ABORT:
                        reservation = None
                        chunks = {}
                    elif request.operation == pb.LOOKUP:
                        if ("entry" not in self.state or
                                self.state["entry"][0].token_digest != request.manifest.token_digest):
                            response.status = pb.NOT_FOUND
                            response.recompute = True
                            response.recompute_ranges = [pb.TokenRange(0, len(request.token_ids))]
                        else:
                            manifest, payload = self.state["entry"]
                            response.manifest = manifest
                            response.lease_id = 72
                            response.hit_tokens = max(1, len(request.token_ids) // 2) if self.partial else len(request.token_ids)
                            response.hit_ranges = [pb.TokenRange(0, response.hit_tokens)]
                            if response.hit_tokens < len(request.token_ids):
                                response.recompute = True
                                response.recompute_ranges = [pb.TokenRange(response.hit_tokens, len(request.token_ids))]
                            response.chunk_indices = list(range(manifest.chunk_count))
                    elif request.operation == pb.GET:
                        payload = self.state["entry"][1]
                        manifest = self.state["entry"][0]
                        start = request.chunk_index * manifest.chunk_bytes
                        response.payload = payload[start:start + manifest.chunk_bytes]
                        response.payload_checksum = zlib.crc32(response.payload) & 0xffffffff
                    body = pb.encode_response(response)
                    reply = b"KVP\x01" + bytes((request.operation | 128, 0, 1, 0, 0))
                    reply += struct.pack(">I", len(body)) + body
                    client.sendall(struct.pack("<I", len(reply)) + reply)
        except (EOFError, OSError):
            pass

    def close(self):
        self._stop.set()
        self._listener.close()
        for client in self._clients:
            try:
                client.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self._thread.join(timeout=2)
        if os.path.lexists(self.path):
            os.unlink(self.path)


def manifest(payload=b"x" * 128, tokens=(1, 2, 3, 4)):
    return pb.TensorManifest(
        version=1, tenant_id="tenant", model_id="model", model_revision="rev",
        tokenizer_revision="tok", cache_format="test", cache_format_version=1,
        token_digest=pb.token_digest(tokens), token_count=len(tokens), layer_count=1,
        dtype=1, shape=[2, 1, len(tokens), 2, 4], axis_order=[1, 2, 3, 5, 6],
        strides_bytes=[32 * len(tokens), 32 * len(tokens), 32, 16, 2],
        layout=1, key_value_packing=1, device_kind=1, tensor_parallel_size=1,
        pipeline_parallel_size=1, payload_bytes=len(payload), chunk_bytes=64,
        chunk_alignment_bytes=64, chunk_count=2,
        payload_digest=__import__("hashlib").sha256(payload).digest())


class UdsSessionTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = str(Path(self.temporary.name) / "bridge.sock")
        self.server = FakeUdsServer(self.path)
        self.addCleanup(self.server.close)
        self.session = Session(self.path, tenant_id="tenant", model_id="model", timeout=0.15)
        self.addCleanup(self.session.close)

    def test_miss_partial_full_and_publication(self):
        value = b"x" * 128
        description = manifest(value)
        with self.assertRaises(SessionError) as missed:
            self.session.lookup(description, (1, 2, 3, 4), exact=True)
        self.assertEqual(missed.exception.response.status, pb.NOT_FOUND)
        committed = self.session.publish(description, pb.payload_chunks(value, 64))
        self.assertEqual(committed.lease_id, 71)
        self.session.release(committed.lease_id)

        self.server.partial = True
        partial = self.session.lookup(description, (1, 2, 3, 4), exact=False)
        self.assertEqual((partial.hit_tokens, partial.recompute), (2, True))
        self.session.release(partial.lease_id)
        self.server.partial = False
        full = self.session.lookup(description, (1, 2, 3, 4), exact=True)
        self.assertEqual(full.hit_tokens, 4)
        self.assertEqual(self.session.get_chunks(full), value)
        self.assertIn(pb.COMMIT, self.server.operations)

    def test_timeout_and_cancellation(self):
        value = b"x" * 128
        self.server.state["entry"] = (manifest(value), value)
        self.server.delay_operation = pb.LOOKUP
        with self.assertRaises(TimeoutError):
            self.session.lookup(manifest(value), (1, 2, 3, 4), deadline=time.monotonic() + 0.03)

        cancel = threading.Event()
        timer = threading.Timer(0.02, cancel.set)
        timer.start()
        self.addCleanup(timer.cancel)
        with self.assertRaises(InterruptedError):
            self.session.lookup(manifest(value), (1, 2, 3, 4), cancel=cancel)

        self.server.delay_operation = None
        self.assertEqual(self.session.negotiate().status, pb.OK)

    def test_reconnect_after_server_restart(self):
        value = b"x" * 128
        state = {"entry": (manifest(value), value)}
        self.server.state = state
        first = self.session.lookup(manifest(value), (1, 2, 3, 4), exact=True)
        self.session.release(first.lease_id)
        self.server.close()
        replacement = FakeUdsServer(self.path, state)
        self.addCleanup(replacement.close)
        self.server = replacement
        result = self.session.lookup(manifest(value), (1, 2, 3, 4), exact=True)
        self.assertEqual(result.hit_tokens, 4)
        self.assertEqual(replacement.operations[:2], [pb.NEGOTIATE, pb.LOOKUP])

    def test_sglang_uds_mode_has_no_process_local_pages(self):
        from kvstore_sglang import KVStoreHiCacheStorage

        config = SimpleNamespace(model_name="model", extra_config={
            "uds_path": self.path, "tenant_id": "tenant", "model_id": "model",
            "namespace": "test", "model_revision": "rev", "tokenizer_revision": "tok",
            "chunk_bytes": 64, "timeout_seconds": 0.15,
        })
        storage = KVStoreHiCacheStorage(config, {})
        self.addCleanup(storage.close)
        self.assertFalse(hasattr(storage, "_pages"))
        page = torch.arange(32, dtype=torch.float16)
        self.assertTrue(storage.set("page-1", page))
        self.assertTrue(storage.exists("page-1"))
        self.assertTrue(torch.equal(storage.get("page-1"), page))

    def test_sglang_rebuilds_query_metadata_after_backend_restart(self):
        from kvstore_sglang import KVStoreHiCacheStorage
        value = torch.arange(32, dtype=torch.float16)
        config = SimpleNamespace(model_name="model", extra_config={
            "uds_path": self.path, "tenant_id": "tenant", "model_id": "model",
            "namespace": "test", "model_revision": "rev", "tokenizer_revision": "tok",
            "chunk_bytes": 64, "timeout_seconds": 0.15,
        })
        first = KVStoreHiCacheStorage(config, {})
        self.addCleanup(first.close)
        self.assertTrue(first.set("persisted", value))
        first.close()
        second = KVStoreHiCacheStorage(config, {})
        self.addCleanup(second.close)
        self.assertFalse(hasattr(second, "_pages"))
        loaded = second.get("persisted", value.clone())
        self.assertTrue(torch.equal(loaded, value))

    def test_sglang_fresh_batch_exists_uses_registered_pool_geometry(self):
        from kvstore_sglang import KVStoreHiCacheStorage

        class Pool:
            page_size = 1

            @staticmethod
            def get_dummy_flat_data_page():
                return torch.zeros(32, dtype=torch.float16)

        value = torch.arange(32, dtype=torch.float16)
        config = SimpleNamespace(model_name="model", extra_config={
            "uds_path": self.path, "tenant_id": "tenant", "model_id": "model",
            "namespace": "test", "model_revision": "rev", "tokenizer_revision": "tok",
            "chunk_bytes": 64, "timeout_seconds": 0.15,
        })
        first = KVStoreHiCacheStorage(config, {})
        self.assertTrue(first.set("fresh", value))
        first.close()
        second = KVStoreHiCacheStorage(config, {})
        self.addCleanup(second.close)
        second.register_mem_pool_host(Pool())
        self.assertEqual(second.batch_exists(["fresh", "missing"]), 1)

    def test_vllm_session_bridge_round_trip(self):
        from kvstore_vllm.bridge import VllmSessionBridge, _Geometry

        session = Session(self.path, tenant_id="tenant", model_id="model", timeout=0.15)
        bridge = VllmSessionBridge(session, _Geometry(
            ("layer.0", "layer.1"), 2, 1, 4, 2, 2), {
                "model_revision": "rev", "tokenizer_revision": "tok", "chunk_bytes": 64,
            })
        self.addCleanup(bridge.close)
        pages = {
            "layer.0": torch.arange(32, dtype=torch.bfloat16).reshape(2, 2, 2, 1, 4),
            "layer.1": torch.arange(32, 64, dtype=torch.bfloat16).reshape(2, 2, 2, 1, 4),
        }
        tokens = (1, 2, 3, 4)
        bridge.publish(tokens, pages)
        self.assertEqual(bridge.lookup("lookup", tokens)["hit_tokens"], len(tokens))
        plan = SimpleNamespace(request_id="load", token_ids=tokens)
        loaded = bridge.get_pages(plan, lambda: False)
        for name in pages:
            self.assertTrue(torch.equal(loaded[name], pages[name]))


if __name__ == "__main__":
    unittest.main()
