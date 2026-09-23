# RDMA replication integration

## Scope

RDMA is a future transport implementation beneath the existing replication state machine. KVRP
negotiation, KVRF logical frame types, checksums, offsets, ACKs, deduplication, timeout behavior, and
full-sync recovery remain unchanged. RDMA cannot bypass snapshot installation or commit ordering.

## Memory registration

The transport owns bounded registration pools. Each region has an explicit owner, generation,
length, access flags, and completion lifetime. Keys and registrations are revoked before memory is
reused. Snapshot and event producers expose immutable chunks; mutable engine storage is never
registered directly. Registration failure applies backpressure or selects the configured fallback.

## Transfer interface

The backend accepts one bounded replication chunk and reports exactly one completion containing its
connection generation, transfer ID, chunk index, byte count, and status. The state machine advances
the sent cursor only after successful completion and advances the replication cursor only after the
matching peer ACK. Disconnect cancels outstanding work and invalidates late completions.

## Fallback and safety

- Capability negotiation explicitly selects RDMA; no endpoint silently assumes support.
- Setup, protection, completion, or path errors close the RDMA transfer boundary. Reconnection uses
  the peer's last acknowledged offset or requests a new snapshot before falling back to TCP.
- Fallback never concatenates a partial RDMA transfer with a new TCP transfer ID.
- Queue depth, registered bytes, retry count, and operation deadlines are bounded and observable.
- Remote addresses and keys are treated as credentials and are never logged.
