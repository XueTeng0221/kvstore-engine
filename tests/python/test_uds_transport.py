import socket
import tempfile
import threading
import unittest
from pathlib import Path
import sys
import os
import subprocess
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[2] / "python"))

from kvstore_vllm.uds import SessionTransport


class UdsTransportTest(unittest.TestCase):
    def test_nul_is_rejected_before_socket_creation(self):
        for path in ("\x00abstract", "/tmp/bridge.sock\x00suffix"):
            transport = SessionTransport(path)
            self.addCleanup(transport.close)
            with patch("kvstore_vllm.uds.socket.socket") as create:
                with self.assertRaisesRegex(ValueError, "NUL"):
                    transport.connect()
                create.assert_not_called()

    def test_ctest_missing_protobuf_fails_instead_of_skipping(self):
        env = dict(os.environ, KVSTORE_UDS_FIXTURE="required-by-ctest")
        result = subprocess.run(
            [sys.executable, "-S", "-m", "unittest", "discover", "-s",
             str(Path(__file__).parent), "-p", "test_uds_bridge_live.py", "-v"],
            env=env, capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CTest live UDS requires", result.stderr)
        self.assertNotIn("skipped=", result.stderr)

    def test_fragmented_response_and_reconnect(self):
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / "session.sock")
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.addCleanup(listener.close)
            listener.settimeout(2)
            listener.bind(path)
            listener.listen(2)

            def serve():
                try:
                    for _ in range(2):
                        client, _ = listener.accept()
                        with client:
                            client.settimeout(2)
                            def read_exact(size):
                                data = bytearray()
                                while len(data) < size:
                                    part = client.recv(size - len(data))
                                    if not part:
                                        raise EOFError("partial request")
                                    data.extend(part)
                                return bytes(data)
                            size = int.from_bytes(read_exact(4), "little")
                            payload = read_exact(size)
                            self.assertEqual(payload[:4], b"KVP\x01")
                            response_body = payload[13:][::-1]
                            envelope = b"KVP\x01" + bytes((payload[4] | 128,)) + payload[5:9] + len(response_body).to_bytes(4, "big") + response_body
                            response = len(envelope).to_bytes(4, "little") + envelope
                            client.sendall(response[:2])
                            client.sendall(response[2:])
                except BaseException as exc:
                    errors.append(exc)
                finally:
                    listener.close()

            errors = []
            thread = threading.Thread(target=serve)
            thread.start()
            self.addCleanup(thread.join, 5)
            transport = SessionTransport(path)
            self.addCleanup(transport.close)
            self.assertEqual(transport.exchange(b"abc", operation=2), b"cba")
            transport.close()
            self.assertEqual(transport.exchange(b"de", operation=2), b"ed")
            transport.close()
            thread.join(timeout=2)
            self.assertFalse(thread.is_alive())
            self.assertEqual(errors, [])


if __name__ == "__main__":
    unittest.main()
