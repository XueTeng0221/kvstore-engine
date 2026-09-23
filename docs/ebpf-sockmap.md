# eBPF/sockmap replication integration

## Scope

eBPF is an optional transport acceleration point after the versioned KVRP handshake. It does not
parse, create, acknowledge, reorder, or deduplicate KVRF frames. The userspace replication state
machine remains authoritative for connection state, transfer boundaries, offsets, event IDs,
heartbeats, timeouts, backpressure, and recovery.

## Attachment point

A future implementation may place authenticated, handshake-complete peer sockets in a sockmap and
use SK_SKB or SK_MSG programs to redirect bytes between explicitly paired sockets. Sockets must be
removed before userspace closes or reuses an fd. Partial frames remain valid and are reassembled by
the userspace codec.

## Constraints and fallback

- Unsupported kernels, verifier rejection, map exhaustion, or program detach must be reported.
- Fallback returns the socket to the configured userspace transport without changing its KVRF byte
  stream or acknowledged offset. Silent frame loss or replay is forbidden.
- Redirected bytes count against the same per-connection output and replication transfer limits.
- Map keys include a userspace connection generation, not an fd alone, to prevent fd-reuse routing.

## Security model

Only sockets accepted by the configured listener and validated by the KVRP role, node ID, version,
and capability checks may enter the map. Programs and maps are pinned with least-privilege ownership;
unprivileged processes cannot update peer mappings. Raw keys, values, credentials, and tensor data
must not be emitted to tracing events. Operational metrics expose counts and offsets only.
