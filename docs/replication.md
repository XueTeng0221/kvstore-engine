# Replication behavior

## LiveSync identity and retention

LiveSync identifies an event by the length-delimited pair `(origin node ID, event ID)`. Relays keep
that identity unchanged while assigning no new logical event identity. The local replication offset
continues to describe ordered application on the single-primary chain.

Each process retains at most 4096 identities for ten minutes. Capacity eviction removes the oldest
identity; TTL expiry makes the identity eligible again. The window is process-local and empty after
restart. A restarted relay first installs an upstream snapshot, resets its local backlog, and
disconnects downstream peers so they reconnect at a clean snapshot boundary.

Events are recorded in the deduplication window before application. Failed application removes
those records so reconnect can retry. Successful events are republished with their original origin
node and event ID. A downstream snapshot reset closes existing connections; reconnect starts a new
transfer ID and prevents old and replacement chunks from mixing.

## Execution backends

The replication executor controls full-sync installation and incremental application. `pthread` is
the bounded worker baseline, `reactor` runs apply on the epoll owner thread, and `io_uring` submits
a real eventfd read operation to Linux SQ/CQ and runs the task after its CQE. `ntyco` runs accepted
tasks as coroutines in the pinned third-party NtyCo runtime. The generic `proactor` adapter remains
available for injected integrations and is not used by the real-backend benchmark.

All modes share queue bounds, drain accepted work during shutdown, reject new work after shutdown,
and isolate callback exceptions. NtyCo has no upstream license file; CMake requires an explicit
local-evaluation acceptance flag and the dependency must not be redistributed without separate
permission. The pinned source contains an author copyright/confidential-use notice, so this
repository treats NtyCo as a local-only, non-redistributable evaluation dependency. No production
deployment or binary redistribution is authorized by this project. Maintenance status is frozen
at commit `72ab5fd04f0c228f464f160aaa521bb791b34aa5`; upstream activity and security response are
not guaranteed and must be re-audited before any release. Executor destruction is owned by its
creator thread; callbacks must not destroy their own executor while running on the io_uring completion thread.
