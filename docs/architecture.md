# KVStore Engine Architecture Baseline

## Scope and topology

Version 0.1 is one writable primary with zero or more read-only replicas. It does not shard data and does not perform automatic leader election. A replica rejects client mutations with `READ_ONLY`; promotion is an explicit operator action after verifying the last applied event offset. P9 may introduce sharding only through a new versioned routing and consistency contract.

The primary linearizes each single-key mutation under the selected engine's write lock. A successful mutation receives one monotonically increasing write-event offset. Reads observe either the state before or after a concurrent mutation, never a partial value. Multi-key batch operations are ordered collections of individually atomic operations; the complete batch is not transactional.

Client success means the mutation is visible in primary memory and accepted by the configured durability policy. `always` requires AOF data sync before success, `everysec` permits up to one flush interval of acknowledged loss, and `no` delegates persistence timing to the operating system. Replication is asynchronous in v0.1. Replica reads may be stale and expose their applied offset. No automatic failover guarantee is made.

Mutations are serialized by the dispatcher. The engine mutation becomes visible first, then one
logical `WriteEvent` is assigned its offset and event ID and submitted to the bounded event queue.
The queue preserves transaction boundaries and applies record/byte backpressure. An ordinary sink
failure rolls the engine mutation back and does not advance the sequence. An indeterminate durable
commit poisons the AOF writer, preserves the visible mutation and advances the sequence so snapshot
and replay cannot apply the event twice; subsequent writes fail until restart. Replay, full-sync and
incremental-sync sources update storage without re-entering the same propagation chain.

The AOF format is a versioned `AOT1` transaction envelope followed by one `AOC1` commit trailer.
CRC32 covers the envelope and all event records. Each event persists source, command, key/value,
origin node, timestamp and event checksum. Recovery applies only complete committed transactions,
rejects discontinuities and corruption, and removes a recognized incomplete header tail before
accepting new writes. A configured `max_aof_bytes` is the v0.1 capacity boundary; rewrite is not yet
implemented.

## Module boundaries

```text
network -> protocol -> command dispatcher -> engine / KVCache service
                                      |-> ordered write-event hub
                                            |-> AOF
                                            |-> replication
```

`src/common` and public headers have no dependency on protocols or networking. Engines store binary byte strings and never parse commands. Protocol parsers produce typed commands and never mutate storage. Persistence and replication consume committed events and cannot feed replayed events back into the same propagation path.

## Thread and ownership model

Network backends own connection state and bounded input/output buffers. Dispatch workers own request context until a response is queued. Each v0.1 engine uses a reader/writer lock around its data structure; returned values and scan records are owned copies, so no lock-protected references escape. Write-event consumers own copied or immutable event payloads.

File descriptors, mappings, threads, and buffers use RAII. Cancellation is represented by `std::stop_token`; operations check it before taking a lock and before mutation. Graceful shutdown stops admission, drains bounded work until the configured deadline, flushes required persistence state, and joins owned threads.

## KVCache direction

Attention KV entries share the byte storage but use canonical metadata including tenant, model and adapter versions, tokenizer/prefix identity, layer range, dtype, shape, layout, device, timestamps, and checksum. Exact and longest-prefix indexes sit above `IEngine`. Tier transitions and framework adapters are P7/P8 work and cannot change P1 engine semantics.

## Capacity and performance baseline

The production acceptance target host is Linux 6.8, 32 physical x86-64 cores, 128 GiB RAM, local PCIe 4 NVMe, and 100 GbE. P10 server-level Release tests use 1 million resident keys, 32-byte keys, 1 KiB values, 80/20 GET/upsert, 64 client connections, and at least 10 measured runs after warmup. P1 uses a smaller single-thread engine microbenchmark only to compare data structures; it does not claim these production targets.

- Hash engine target: at least 500k operations/s, p99 below 2 ms at 70% configured capacity.
- Ordered engines target: at least 150k operations/s, p99 below 5 ms under the same load.
- Idle recovery target: restore 100 GiB in 120 seconds on the reference NVMe.
- Asynchronous replication target: p99 apply lag below 50 ms on an unloaded 100 GbE link.
- KVCache targets are established in P8 against real vLLM/SGLang traces; no cache speedup is claimed before those measurements.

These are acceptance targets, not results from the development machine. P1 records local comparative results with exact hardware and workload.
