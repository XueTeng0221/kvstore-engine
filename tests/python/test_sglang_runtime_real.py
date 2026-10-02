import sys
import unittest
import json
import tempfile
import queue
from pathlib import Path
from types import SimpleNamespace

import torch

sys.path.insert(0, str(Path(__file__).parents[2] / "python"))


class _Pool:
    def __init__(self):
        self.pages = {}
        self.page_size = 2

    def get_data_page(self, index):
        return self.pages[index]

    def set_from_flat_data_page(self, index, page):
        self.pages[index] = page.clone()


class SglangRuntimeTest(unittest.TestCase):
    def test_dynamic_factory_registration_and_restart_page_hit(self):
        from kvstore_sglang import KVStoreHiCacheStorage
        from sglang.srt.mem_cache.hicache_storage import HiCacheStorage, HiCacheStorageConfig
        from sglang.srt.mem_cache.storage.backend_factory import StorageBackendFactory

        self.assertTrue(issubclass(KVStoreHiCacheStorage, HiCacheStorage))
        config = HiCacheStorageConfig(
            tp_rank=0, tp_size=1, pp_rank=0, pp_size=1,
            attn_cp_rank=0, attn_cp_size=1, is_mla_model=False,
            enable_storage_metrics=False, is_page_first_layout=True,
            model_name="Qwen2.5-0.5B",
            extra_config={
                "backend_name": "kvstore",
                "module_path": "kvstore_sglang.hicache",
                "class_name": "KVStoreHiCacheStorage",
                "namespace": "qwen-e2e",
            },
        )
        storage = StorageBackendFactory.create_backend("dynamic", config, _Pool())
        page = torch.arange(128, dtype=torch.bfloat16)
        self.assertTrue(storage.set("radix-page-0", page))
        self.assertEqual(storage.batch_exists(["radix-page-0", "miss"]), 1)
        self.assertTrue(torch.equal(storage.get("radix-page-0"), page))
        storage.close()

    def test_v2_registered_pool_transfer(self):
        from kvstore_sglang import KVStoreHiCacheStorage
        storage = KVStoreHiCacheStorage(None, None)
        pool = _Pool()
        pool.pages[4] = torch.arange(64, dtype=torch.float16)
        storage.register_mem_host_pool_v2(pool, "kv")
        self.assertIs(storage.registered_pools["kv"], pool)

    def test_v2_auxiliary_pool_hit_accounting(self):
        from kvstore_sglang import KVStoreHiCacheStorage
        storage = KVStoreHiCacheStorage(None, None)
        storage.set("main-0", torch.tensor([1]))
        storage.set("aux-0", torch.tensor([2]))
        result = storage.batch_exists_v2(
            ["main-0"],
            [type("Transfer", (), {"name": "swa", "keys": ["aux-0"]})()],
        )
        self.assertEqual(result.kv_hit_pages, 1)
        self.assertEqual(result.extra_pool_hit_pages, {"swa": 1})

    def test_v1_flat_indices_use_page_boundaries(self):
        from kvstore_sglang import KVStoreHiCacheStorage
        storage = KVStoreHiCacheStorage(None, None)
        pool = _Pool()
        storage.register_mem_pool_host(pool)
        pool.pages[10] = torch.tensor([1])
        pool.pages[20] = torch.tensor([2])
        self.assertEqual(
            storage.batch_set_v1(["a", "b"], torch.tensor([10, 11, 20, 21])),
            [True, True],
        )
        pool.pages.clear()
        self.assertEqual(
            storage.batch_get_v1(["a", "b"], torch.tensor([30, 31, 40, 41])),
            [True, True],
        )
        self.assertEqual(sorted(pool.pages), [30, 40])
        self.assertEqual(
            storage.batch_get_v1(["a", "b"], torch.tensor([30, 31])),
            [False, False],
        )

    def test_stock_extra_info_cancellation_stops_v2_transfer(self):
        from kvstore_sglang import KVStoreHiCacheStorage
        from sglang.srt.mem_cache.hicache_storage import (
            HiCacheStorageExtraInfo, PoolHitPolicy)

        storage = KVStoreHiCacheStorage(None, None)
        pool = _Pool()
        pool.pages[0] = torch.tensor([1])
        storage.register_mem_host_pool_v2(pool, "kv")
        info = HiCacheStorageExtraInfo(
            extra_info={"cancelled": lambda: True})
        transfer = type("Transfer", (), {
            "name": "kv", "keys": ["page"],
            "host_indices": torch.tensor([0, 1]),
        })()
        self.assertEqual(storage.batch_get_v2([transfer], info), {"kv": [False]})

        info = HiCacheStorageExtraInfo()
        transfer.hit_policy = PoolHitPolicy.TRAILING_PAGES
        storage.set("page", torch.tensor([1]))
        self.assertEqual(
            storage.batch_exists_v2(["page"], [transfer], info).extra_pool_hit_pages,
            {"kv": 1})

        self.assertEqual(
            storage.batch_exists_v2([], [transfer], info).extra_pool_hit_pages,
            {"kv": 0})

        for key in ("k0", "k1", "k2", "k3", "a0", "a1"):
            storage.set(key, torch.tensor([1]))
        transfer.keys = ["a0", "a1"]
        self.assertEqual(
            storage.batch_exists_v2(["k0", "k1", "k2", "k3"], [transfer], info)
            .extra_pool_hit_pages,
            {"kv": 4})
        storage._pages.pop("default:a1")
        self.assertEqual(
            storage.batch_exists_v2(["k0", "k1", "k2", "k3"], [transfer], info)
            .extra_pool_hit_pages,
            {"kv": 3})

    def test_stock_controller_operation_is_forwarded_as_real_callback(self):
        from kvstore_sglang import KVStoreHiCacheStorage
        import kvstore_sglang.hicache as adapter
        from sglang.srt.managers import cache_controller

        KVStoreHiCacheStorage(None, None)
        terminated = {"value": False}
        token = adapter._stock_cancellation.set(lambda: terminated["value"])
        try:
            info = cache_controller.HiCacheStorageExtraInfo(prefix_keys=["p"])
        finally:
            adapter._stock_cancellation.reset(token)
        self.assertIn("cancelled", info.extra_info)
        self.assertIn("cancel_event", info.extra_info)
        self.assertFalse(info.extra_info["cancelled"]())
        terminated["value"] = True
        self.assertTrue(info.extra_info["cancelled"]())
        self.assertTrue(info.extra_info["cancel_event"].is_set())

    def test_stock_prefetch_queue_path_delivers_cancellation_to_backend(self):
        from kvstore_sglang import KVStoreHiCacheStorage
        import kvstore_sglang.hicache as adapter
        from sglang.srt.managers import cache_controller
        from sglang.srt.managers.cache_controller import (
            HiCacheController, PrefetchOperation)

        KVStoreHiCacheStorage(None, None)
        operation = PrefetchOperation("request-1", [1, 2])
        operation.hash_value = ["page-1"]
        operation.host_indices = torch.tensor([0])
        operation.prefix_keys = []
        controller = object.__new__(HiCacheController)
        controller.page_size = 1
        controller.prefetch_sync_queue = queue.Queue()
        observed = {}

        def page_get(_operation, _keys, _indices, extra_info):
            observed["cancelled"] = extra_info.extra_info["cancelled"]
            observed["event"] = extra_info.extra_info["cancel_event"]
            operation.mark_terminate()
            return 0

        controller.page_get_func = page_get
        adapter._install_stock_cancellation_bridge()
        self.assertEqual(controller._page_transfer(operation), 0)
        self.assertTrue(observed["cancelled"]())
        self.assertTrue(observed["event"].is_set())

        installed = cache_controller.HiCacheController._page_transfer
        adapter._install_stock_cancellation_bridge()
        self.assertIs(cache_controller.HiCacheController._page_transfer, installed)
        adapter._restore_stock_cancellation_bridge()
        self.assertIsNot(cache_controller.HiCacheController._page_transfer, installed)
        adapter._install_stock_cancellation_bridge()

    def test_audit_publish_uses_bounded_append(self):
        from kvstore_sglang import KVStoreHiCacheStorage

        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / "audit.jsonl")
            storage = KVStoreHiCacheStorage(
                SimpleNamespace(extra_config={"audit_path": path}),
                None)
            storage._record("publish", payload_bytes=7)
            records = [json.loads(line) for line in Path(path).read_text().splitlines()]
            self.assertEqual(records, [{"operation": "publish", "payload_bytes": 7}])

    def test_audit_failure_does_not_escape_operation(self):
        from kvstore_sglang import KVStoreHiCacheStorage

        storage = KVStoreHiCacheStorage(
            SimpleNamespace(extra_config={"audit_path": object()}), None)
        storage._record("publish", payload_bytes=object())
if __name__ == "__main__":
    unittest.main()
