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

    def _connector(self, kv_cache_config=None):
        from kvstore_vllm import KVStoreConnector
        from vllm.distributed.kv_transfer.kv_connector.v1.base import KVConnectorRole
        config = SimpleNamespace(
            kv_transfer_config=SimpleNamespace(is_kv_producer=True),
            cache_config=SimpleNamespace(block_size=16),
        )
        return KVStoreConnector(config, KVConnectorRole.WORKER, kv_cache_config)

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

    def test_runtime_layer_count_is_not_model_hardcoded(self):
        import torch

        config = SimpleNamespace(
            kv_cache_groups=[SimpleNamespace(
                layer_names=["layer.0", "layer.1"], enable_kv_transfer=True
            )]
        )
        connector = self._connector(config)
        caches = {
            f"layer.{layer}": torch.zeros(
                (1, 1, 1, 1, 1), device="cuda", dtype=torch.bfloat16
            )
            for layer in range(2)
        }
        connector.register_kv_caches(caches)
        connector.shutdown()

    def test_runtime_layer_membership_and_shapes_are_validated(self):
        import torch

        config = SimpleNamespace(
            kv_cache_groups=[SimpleNamespace(
                layer_names=["layer.0", "layer.1"], enable_kv_transfer=True
            )]
        )
        connector = self._connector(config)
        valid = {
            f"layer.{layer}": torch.zeros(
                (1, 1, 1, 1, 1), device="cuda", dtype=torch.bfloat16
            )
            for layer in range(2)
        }
        with self.assertRaises(ValueError):
            connector.register_kv_caches({"layer.0": valid["layer.0"]})
        with self.assertRaises(ValueError):
            connector.register_kv_caches({**valid, "layer.2": valid["layer.0"]})
        malformed = dict(valid)
        malformed["layer.1"] = torch.zeros((0, 1), device="cuda", dtype=torch.bfloat16)
        with self.assertRaises(ValueError):
            connector.register_kv_caches(malformed)

        config.kv_cache_groups.append(SimpleNamespace(
            layer_names=["layer.2"], enable_kv_transfer=False
        ))
        connector = self._connector(config)
        valid["layer.2"] = torch.zeros(
            (1, 1, 1, 1, 1), device="cuda", dtype=torch.bfloat16
        )
        connector.register_kv_caches(valid)
        connector.shutdown()

    def test_each_runtime_cache_tensor_is_validated(self):
        import torch

        connector = self._connector()
        valid = {
            f"layer.{layer}": torch.zeros(
                (1, 1, 1, 1, 1), device="cuda", dtype=torch.bfloat16
            )
            for layer in range(2)
        }
        cases = {
            "non_tensor": {**valid, "layer.1": object()},
            "cpu": {**valid, "layer.1": torch.zeros((1, 1, 1), dtype=torch.bfloat16)},
            "dtype": {**valid, "layer.1": torch.zeros(
                (1, 1, 1, 1, 1), device="cuda", dtype=torch.float16
            )},
            "rank": {**valid, "layer.1": torch.zeros(
                (1,), device="cuda", dtype=torch.bfloat16
            )},
            "zero_dimension": {**valid, "layer.1": torch.zeros(
                (1, 0, 1), device="cuda", dtype=torch.bfloat16
            )},
        }
        for name, caches in cases.items():
            with self.subTest(name=name), self.assertRaises(ValueError):
                connector.register_kv_caches(caches)

    def test_none_cache_config_keeps_runtime_compatibility(self):
        import torch

        connector = self._connector()
        connector.register_kv_caches({
            f"layer.{layer}": torch.zeros(
                (1, 1, 1, 1, 1), device="cuda", dtype=torch.bfloat16
            )
            for layer in range(2)
        })
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
