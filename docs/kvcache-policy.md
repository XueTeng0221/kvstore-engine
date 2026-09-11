# KVCache Policy and Load Scheduling

P7.4 provides `CachePolicyEngine` and `LoadScheduler` in
`kvstore/kvcache/policy.hpp`. The policy object is synchronous and does not own
the tiered store. Callers report an observation after a lookup and use
`ShouldAdmit` and `SelectVictims` before calling P7.3 operations.

`CachePolicyEngine` retains at most `max_tracked_objects` records. Each record
contains recency, saturating frequency, size, load cost, recompute cost, reuse
distance, and workload class. The explanation exposes each term. LRU uses
`1/(1+age)`; GDSF uses
`aging + frequency * workload_weight * (load_cost + recompute_cost) * reuse / size`,
where `reuse` is `1/(1+reuse_distance)`. Aging advances to the last selected
GDSF victim score. Recency remains in the explanation and breaks equal GDSF
scores. `ShouldAdmit` compares a candidate against bounded resident victim
scores when free space is insufficient; callers confirm migration outcomes with
`SetResident`.
The victim scan is capped by `max_scan_objects`, and ties are resolved by key.
Prefill, decode, and low-reuse observations use independently configurable
weights. A low-reuse object therefore receives less admission and retention
value than an otherwise identical reusable object.

`LoadScheduler` has bounded pending requests, active-load concurrency, and
reserved estimated I/O bytes. `Submit` returns `kQueueFull`, `kDeadline`, or
`kIoBudget` as a fast miss when work cannot be accepted. `Pop` selects the
highest priority and earliest deadline within a tenant, then visits non-empty
tenant queues in deterministic round-robin order with a bounded configurable
quantum. Within that tenant, expired requests are removed and requests blocked
by the current I/O reservation are skipped before priority/deadline ordering is
applied to eligible work. If no eligible request exists, `Pop` can return the
first expired request with `dispatch() == false`. `Complete` releases the exact
reservation and `Cancel` removes pending work. No scheduler thread is created.
Deadline feasibility compares unsigned clock-tick distance and never subtracts
signed time points. Empty tenant queues are pruned after dispatch/cancellation
and before selection with cursor adjustment; `tenant_queues` exposes the bound.
The optional `fail_activation` test flag is data-only and fails before active
state mutation. Scheduler methods never invoke caller callbacks.

Metrics are fixed-width counters and bounded gauges. They do not perform
persistence, training, or unbounded work on the cache hot path. Unit tests use
logical time and a deterministic trace; the GDSF assertion compares retained
cost-aware value and does not claim wall-clock performance.

The policy and scheduler serialize their own state and return metric snapshots
by value. JSON configuration selects `lru` or `gdsf` and sets tracked-object/scan
bounds, workload weights, pending/concurrency/I/O limits, and tenant quantum.
The server does not construct the P7 policy path yet; P7.5 owns mapping the
validated `KvCacheConfig` into these objects when it integrates request lookup
with `TieredStore`.

## Deterministic trace

`kvcache_policy_benchmark` models a two-object cache and 74 requests. One object
has recompute latency/cost 100, while stable and scanning objects cost 1. Reported
latency is model time, not measured wall-clock time. The executable returns
nonzero unless GDSF has lower miss cost, GDSF p95 is at most 1, and LRU p95 is
at least 100. Raw miss counts remain visible because retaining expensive objects
can intentionally exchange more cheap misses for lower cost and tail latency.
Release results are recorded in
`docs/p7-policy-benchmark.md`.
