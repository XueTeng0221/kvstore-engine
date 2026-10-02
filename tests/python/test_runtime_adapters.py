import sys
import unittest
import threading
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).parents[2] / "python"))


class RuntimeAdapterTest(unittest.TestCase):
    def test_vllm_connector_constructs_and_collects_layers(self):
        from kvstore_vllm import KVStoreConnector
        from vllm.distributed.kv_transfer.kv_connector.v1.base import KVConnectorRole

        config = SimpleNamespace(
            kv_transfer_config=SimpleNamespace(is_kv_producer=True),
        )
        connector = KVStoreConnector(config, KVConnectorRole.WORKER, None)
        calls = []
        connector.bind_store(lambda key, payload: calls.append((key, payload)))
        connector.begin_save("request-1", [1, 2])
        connector.save_kv_layer_for_request("request-1", "layer0", "layer0")
        connector.save_kv_layer_for_request("request-1", "layer1", "layer1")
        connector.wait_for_save()
        self.assertEqual(len(calls), 1)
        self.assertEqual((calls[0][0], sorted(calls[0][1])),
                         ((1, 2), ["layer0", "layer1"]))

    def test_vllm_store_failure_is_retryable(self):
        from kvstore_vllm import KVStoreConnector
        from vllm.distributed.kv_transfer.kv_connector.v1.base import KVConnectorRole

        config = SimpleNamespace(
            kv_transfer_config=SimpleNamespace(is_kv_producer=True),
        )
        connector = KVStoreConnector(config, KVConnectorRole.WORKER, None)
        calls = []

        def put(key, payload):
            calls.append((key, payload))
            if len(calls) == 1:
                raise RuntimeError("temporary store failure")

        connector.bind_store(put)
        connector.begin_save("request-2", [3])
        connector.save_kv_layer("layer0", "value", None)
        connector.wait_for_save()
        connector.wait_for_save()
        self.assertEqual(len(calls), 2)

    def test_finish_before_save_keeps_transaction(self):
        from kvstore_vllm import KVStoreConnector
        from vllm.distributed.kv_transfer.kv_connector.v1.base import KVConnectorRole
        config = SimpleNamespace(kv_transfer_config=SimpleNamespace(is_kv_producer=True))
        connector = KVStoreConnector(config, KVConnectorRole.WORKER, None)
        calls = []
        connector.bind_store(lambda key, payload: calls.append((key, payload)))
        connector.begin_save("late", [9])
        connector.request_finished("late", [])
        connector.save_kv_layer_for_request("late", "layer", "value")
        connector.wait_for_save()
        self.assertEqual(len(calls), 1)

    def test_request_finished_preserves_forward_save_payload(self):
        from kvstore_vllm import KVStoreConnector
        from vllm.distributed.kv_transfer.kv_connector.v1.base import KVConnectorRole
        config = SimpleNamespace(kv_transfer_config=SimpleNamespace(is_kv_producer=True))
        connector = KVStoreConnector(config, KVConnectorRole.WORKER, None)
        calls = []
        connector.bind_store(lambda key, payload: calls.append((key, payload)))
        request = SimpleNamespace(request_id="forward-first", prompt_token_ids=[7, 8])
        connector.begin_save(request.request_id, request.prompt_token_ids)
        connector.save_kv_layer_for_request(request.request_id, "layer", "value")
        connector.request_finished(request, [1])
        connector.wait_for_save()
        self.assertEqual(calls, [((7, 8), {"layer": "value"})])

    def test_request_scope_routes_interleaved_layers(self):
        from kvstore_vllm import KVStoreConnector
        from vllm.distributed.kv_transfer.kv_connector.v1.base import KVConnectorRole
        config = SimpleNamespace(kv_transfer_config=SimpleNamespace(is_kv_producer=True))
        connector = KVStoreConnector(config, KVConnectorRole.WORKER, None)
        calls = []
        connector.bind_store(lambda key, payload: calls.append((key, payload)))
        connector.begin_save("a", [1])
        connector.begin_save("b", [2])
        with connector.request_scope("a"):
            connector.save_kv_layer("layer", "A", None)
        with connector.request_scope("b"):
            connector.save_kv_layer("layer", "B", None)
        connector.wait_for_save()
        self.assertEqual(sorted(value for _, payload in calls for value in payload.values()), ["A", "B"])

    def test_replacement_drains_in_one_wait(self):
        connector, calls = self._connector_with_calls()
        connector.begin_save("same", [1])
        connector.save_kv_layer_for_request("same", "layer", "old")
        connector.begin_save("same", [2])
        connector.save_kv_layer_for_request("same", "layer", "new")
        connector.wait_for_save()
        self.assertEqual([key for key, _ in calls], [(2,)])

    def test_concurrent_wait_does_not_duplicate(self):
        connector, calls = self._connector_with_calls()
        connector.begin_save("once", [4])
        connector.save_kv_layer_for_request("once", "layer", "value")
        threads = [threading.Thread(target=connector.wait_for_save) for _ in range(2)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=2)
        self.assertEqual(len(calls), 1)

    def test_failure_then_finish_keeps_retryable_transaction(self):
        connector, calls = self._connector_with_calls()
        connector.begin_save("retry", [5])
        connector.save_kv_layer_for_request("retry", "layer", "value")
        connector.bind_store(lambda key, payload: (_ for _ in ()).throw(RuntimeError("fail")))
        errors = connector.wait_for_save()
        self.assertIn("retry", errors)
        connector.request_finished("retry", [])
        connector.bind_store(lambda key, payload: calls.append((key, payload)))
        connector.wait_for_save()
        self.assertEqual(len(calls), 1)

    def test_scheduler_metadata_initializes_worker_publication(self):
        from kvstore_vllm import KVStoreConnector
        from vllm.distributed.kv_transfer.kv_connector.v1.base import KVConnectorRole

        config = SimpleNamespace(
            kv_transfer_config=SimpleNamespace(
                is_kv_producer=True, kv_connector_extra_config={}
            ),
            cache_config=SimpleNamespace(block_size=16),
        )
        scheduler = KVStoreConnector(config, KVConnectorRole.SCHEDULER, None)
        worker = KVStoreConnector(config, KVConnectorRole.WORKER, None)
        request = SimpleNamespace(request_id="framework", prompt_token_ids=list(range(16)))
        scheduler.on_new_request(request)
        scheduler.update_state_after_alloc(request, [3], 0)
        metadata = scheduler.build_connector_meta(None)
        self.assertTrue(scheduler.request_finished(request, [3])[0])
        worker.bind_connector_metadata(metadata)
        worker.start_load_kv(None)
        calls = []
        worker.bind_store(lambda key, payload: calls.append((key, payload)))
        worker.save_kv_layer_for_request("framework", "layer", "value")
        worker.wait_for_save()
        self.assertEqual(calls, [(tuple(range(16)), {"layer": "value"})])

    def test_scheduler_publication_uses_complete_blocks_only(self):
        from kvstore_vllm import KVStoreConnector
        from vllm.distributed.kv_transfer.kv_connector.v1.base import KVConnectorRole

        config = SimpleNamespace(
            kv_transfer_config=SimpleNamespace(
                is_kv_producer=True, kv_connector_extra_config={}),
            cache_config=SimpleNamespace(block_size=16),
        )
        scheduler = KVStoreConnector(config, KVConnectorRole.SCHEDULER, None)
        request = SimpleNamespace(request_id="unaligned", prompt_token_ids=list(range(18)))
        scheduler.on_new_request(request)
        scheduler.update_state_after_alloc(request, [3, 4], 0)
        metadata = scheduler.build_connector_meta(None)
        self.assertEqual(len(metadata.saves), 1)
        self.assertEqual(metadata.saves[0].token_ids, tuple(range(16)))
        self.assertEqual(metadata.saves[0].block_ids, (3,))

    def test_full_external_hit_propagates_skip_save_metadata(self):
        from kvstore_vllm import KVStoreConnector
        from vllm.distributed.kv_transfer.kv_connector.v1.base import KVConnectorRole

        config = SimpleNamespace(
            kv_transfer_config=SimpleNamespace(
                is_kv_producer=True, kv_connector_extra_config={}),
            cache_config=SimpleNamespace(block_size=16),
        )
        connector = KVStoreConnector(config, KVConnectorRole.SCHEDULER, None)
        request = SimpleNamespace(request_id="full", prompt_token_ids=list(range(16)))
        connector.on_new_request(request)
        connector.update_state_after_alloc(request, [3], 0)
        connector._external_full_hits.add("full")
        metadata = connector.build_connector_meta(None)
        self.assertTrue(metadata.saves[0].skip_save)
        worker = KVStoreConnector(config, KVConnectorRole.WORKER, None)
        worker.bind_connector_metadata(
            type("Metadata", (), {"loads": (), "saves": metadata.saves})())
        worker.start_load_kv(None)
        calls = []
        worker.bind_store(lambda key, payload: calls.append((key, payload)))
        worker.save_kv_layer_for_request("full", "layer", "value")
        worker.wait_for_save()
        self.assertEqual(calls, [])

        connector = KVStoreConnector(config, KVConnectorRole.SCHEDULER, None)
        connector.on_new_request(request)
        connector.update_state_after_alloc(request, [3, 4], 8)
        metadata = connector.build_connector_meta(None)
        self.assertFalse(metadata.saves[0].skip_save)

        worker = KVStoreConnector(config, KVConnectorRole.WORKER, None)
        worker.bind_connector_metadata(
            type("Metadata", (), {"loads": metadata.loads, "saves": metadata.saves})())
        worker.start_load_kv(None)
        calls = []
        worker.bind_store(lambda key, payload: calls.append((key, payload)))
        worker.save_kv_layer_for_request("full", "layer", "value")
        worker.wait_for_save()
        self.assertEqual(calls, [(tuple(range(16)), {"layer": "value"})])

    def test_external_load_uses_runtime_block_size(self):
        from kvstore_vllm import KVStoreConnector
        from vllm.distributed.kv_transfer.kv_connector.v1.base import KVConnectorRole

        config = SimpleNamespace(
            kv_transfer_config=SimpleNamespace(
                is_kv_producer=True, kv_connector_extra_config={}),
            cache_config=SimpleNamespace(block_size=32),
        )
        connector = KVStoreConnector(config, KVConnectorRole.WORKER, None)
        request = SimpleNamespace(request_id="block32", prompt_token_ids=list(range(64)))
        connector.get_num_new_matched_tokens = lambda request, computed: (64, True)
        connector._load_plans[request.request_id] = type(
            "Plan", (), {"generation": 1})()
        connector.update_state_after_alloc(request, [4, 5, 6], 64)
        plan = connector._load_plans[request.request_id]
        self.assertEqual(plan.block_ids, (4, 5))

    def _connector_with_calls(self):
        from kvstore_vllm import KVStoreConnector
        from vllm.distributed.kv_transfer.kv_connector.v1.base import KVConnectorRole
        config = SimpleNamespace(kv_transfer_config=SimpleNamespace(is_kv_producer=True))
        connector = KVStoreConnector(config, KVConnectorRole.WORKER, None)
        calls = []
        connector.bind_store(lambda key, payload: calls.append((key, payload)))
        return connector, calls

    def test_sglang_deadline_is_checked_before_transport(self):
        from kvstore_sglang import KVStoreHiCacheStorage

        storage = KVStoreHiCacheStorage(
            lambda *args: {"status": "ok"}, lambda *args: None,
            lambda *args: None, lambda *args: None)
        self.assertEqual(
            storage.lookup([1], deadline=0)["status"], "deadline_exceeded")

    def test_sglang_cancel_and_publish_cleanup(self):
        calls = []
        from kvstore_sglang import KVStoreHiCacheStorage

        storage = KVStoreHiCacheStorage(
            lambda *args: {"status": "ok"},
            lambda manifest, chunks: calls.append((manifest, chunks)),
            lambda lease: calls.append(("release", lease)),
            lambda reservation: calls.append(("abort", reservation)),
        )
        self.assertEqual(storage.lookup([1], cancelled=True)["status"], "cancelled")
        storage.publish({"reservation_id": 7}, [b"x"])
        self.assertEqual(calls[0][0]["reservation_id"], 7)


if __name__ == "__main__":
    unittest.main()
