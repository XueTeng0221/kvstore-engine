import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).parents[2] / "python"))


class _Transport:
    def __init__(self, pages):
        self.pages = pages
        self.cancelled = []

    def get_pages(self, plan, cancelled):
        if cancelled():
            raise RuntimeError("cancelled before DMA")
        return self.pages

    def cancel(self, request_id):
        self.cancelled.append(request_id)


class VllmGpuRuntimeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import torch
        if not torch.cuda.is_available():
            raise RuntimeError("P8.2 requires a CUDA GPU")

    def _connector(self):
        from kvstore_vllm import KVStoreConnector
        from vllm.distributed.kv_transfer.kv_connector.v1.base import KVConnectorRole
        config = SimpleNamespace(
            kv_transfer_config=SimpleNamespace(is_kv_producer=True),
            cache_config=SimpleNamespace(block_size=16),
        )
        return KVStoreConnector(config, KVConnectorRole.WORKER, None)

    def test_real_paged_h2d_injection_and_event_fence(self):
        import torch
        from kvstore_vllm.connector import _Load, _Metadata

        connector = self._connector()
        caches = {
            f"layer.{layer}": torch.zeros(
                (4, 2, 16, 2, 64), device="cuda", dtype=torch.bfloat16
            )
            for layer in range(24)
        }
        connector.register_kv_caches(caches)
        pages = {
            name: torch.full(
                (2, 2, 16, 2, 64), layer + 1, dtype=torch.bfloat16,
                pin_memory=True,
            )
            for layer, name in enumerate(caches)
        }
        transport = _Transport(pages)
        connector.bind_transport(transport)
        connector.bind_connector_metadata(
            _Metadata((_Load("load", 32, (3, 1), 1),))
        )
        connector.start_load_kv(None)
        for name in caches:
            connector.wait_for_layer_load(name)
        torch.cuda.current_stream().synchronize()
        for layer, tensor in enumerate(caches.values()):
            self.assertTrue(torch.equal(tensor[3].cpu(), pages[f"layer.{layer}"][0]))
            self.assertTrue(torch.equal(tensor[1].cpu(), pages[f"layer.{layer}"][1]))
            self.assertEqual(torch.count_nonzero(tensor[0]).item(), 0)
        connector.cancel("load")
        self.assertEqual(transport.cancelled, ["load"])
        connector.shutdown()

    def test_real_d2h_copy_waits_for_compute_and_survives_cancel(self):
        import torch
        from kvstore_vllm.connector import _Load

        connector = self._connector()
        caches = {
            f"layer.{layer}": torch.zeros(
                (4, 2, 16, 2, 64), device="cuda", dtype=torch.bfloat16
            )
            for layer in range(24)
        }
        connector.register_kv_caches(caches)
        connector.bind_store(lambda key, payload: None)
        connector.begin_save("save", range(32))
        save = connector._load["save"]
        connector._load_plans["save"] = _Load("save", 32, (2, 0), save.generation)
        with connector.request_scope("save"):
            for layer, (name, tensor) in enumerate(caches.items()):
                tensor[2].fill_(layer + 1)
                tensor[0].fill_(layer + 2)
                connector.save_kv_layer(name, tensor, None)
        connector.cancel("save")
        errors = connector.wait_for_save()
        self.assertIn("save", errors)
        connector.shutdown()


if __name__ == "__main__":
    unittest.main()
