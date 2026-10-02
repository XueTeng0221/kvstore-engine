import sys
import unittest
from pathlib import Path

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


if __name__ == "__main__":
    unittest.main()
