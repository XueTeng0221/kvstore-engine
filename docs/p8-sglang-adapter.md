# SGLang adapter (P8.3)

The supported baseline is SGLang `0.5.19` with Qwen2.5-0.5B. This adapter models
the dynamic `HiCacheStorage` `interface_v1` boundary without importing SGLang or
CUDA headers. A radix node is represented by its complete token prefix; its model,
tokenizer, layer, head geometry, dtype, device and block metadata are mapped to
the P8.1 canonical manifest. Consequently tenant/model/revision and tensor layout
cannot accidentally cross a cache entry.

`HiCacheStorage` sends serialized P8.1 requests. Lookup returns miss, partial hit,
or full hit ranges. Publish performs reserve, per-chunk CRC checked PUT, and commit;
any failed PUT aborts the reservation. Release only drops the session lease.
Deadline and cancellation are admission failures and callers must recompute. A
bad GET checksum is a fallback condition, never usable cache data. Reconnects must
negotiate again; published resident objects remain discoverable after a server
restart when the backing server has restored them.

This is a framework-independent contract/mock path. It does not claim Python
HiCacheStorage loading, radix insertion, CUDA DMA, or SGLang/GPU end-to-end tests.
