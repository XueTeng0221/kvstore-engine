# P8.1 Integration Protocol

P8.1 remains in progress pending independent audit. The framework-neutral sidecar
contract is implemented by generated Protobuf messages and `Session::Exchange`.
`kvstore_mock_client` actually serializes requests, dispatches their wire bytes,
and decodes serialized replies. It does not open a socket or invoke a GPU runtime.
The future production transport is a Unix domain socket; its listener, peer
authentication and framework adapters are not claimed here.

## Build and dependency

CMake FetchContent builds Protobuf 3.21.12 (upstream tag v21.12, BSD-3-Clause),
pinned by SHA256. Protobuf is required for interoperable generated parsing and
serialization, not a hand-written approximation. System protoc is unnecessary.
Generated files live only in the build directory. Generated parsing code is
instrumented in sanitizer builds; upstream library/compiler targets do not inherit
the project's strict warning flags or sanitizer flags. Upstream GCC reports an
AlignFail noreturn warning; project targets remain warning-as-error.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DKVSTORE_BUILD_TESTS=ON
cmake --build build -j4
build/kvstore_mock_client
ctest --test-dir build --output-on-failure -R 'Integration(Codec|Session|Mock)'
```

## Framing and schema

The normative schema is `proto/kvstore_integration_v1.proto`. Each frame is:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 3 | ASCII KVP |
| 3 | 1 | framing version 1 |
| 4 | 1 | operation enum; response sets bit 7 |
| 5 | 2 | big-endian major, 1 |
| 7 | 2 | big-endian minor, 0 |
| 9 | 4 | big-endian Protobuf payload length |
| 13 | N | generated Request or Response |

The 16 MiB maximum includes all 13 header bytes. Header operation must match the
decoded message and direction. Unknown operations, newer versions, malformed
Protobuf, truncated whole frames and invalid tensor enum values are rejected.
Unknown Protobuf fields are accepted for additive evolution; changing semantics
requires version negotiation, and incompatible changes require a new major.
No alternate opaque binary manifest exists: all 34 manifest fields are explicit,
including model/adapter/tokenizer revisions, digests, tensor layout/axes/strides,
parallel ranks, device, chunk geometry, timestamps and payload identity.

`Feed` accepts fragments and coalesced frames. Before copying it checks that the
sum of existing buffered bytes and supplied bytes is at most 16 MiB. Each call
emits at most 4096 frames, bounding object overhead as well as payload. The caller
must use bounded reads and close/discard parser state on any Feed error; frames
from an erroneous call must not be dispatched. This is intentionally an aggregate
per-call limit, not permission to feed an arbitrarily large pipeline in one call.

## Identity and lifetime

The transport must authenticate the peer and pass its authorized tenant/model
to the Session constructor. Negotiation cannot select or change this identity.
Every operation carries a matching trace with a nonempty request ID. A session
is noncopyable and nonmovable; calls and disconnect are mutex-serialized. The
registry and shared MatchIndex must outlive every session. Session destruction
must not race access to the destroyed C++ object.

Each session has at most 64 pending reservations and 64 leases. Reservation IDs
are registry-generated but usable only by the session which reserved them, even
between sessions for the same tenant. Immutable manifest identity is checked
before Reserve and Lookup; Get accepts only a session lease, not arbitrary tenant
metadata. Lease IDs are process-unique monotonic values, never pointers. Neither
kind of ID is an authentication credential; transport identity remains required.

Disconnect/destruction aborts all owned reservations and releases all owned pins,
and disconnect is terminal. Reservation tracking storage is preallocated; manifest
copying happens before registry acquisition. The regression hook exercises rollback
at the acquisition/tracking boundary. Registry Abort and staged-index rollback
perform no allocations, allowing cleanup under allocation pressure.

## Operations

| Operation | Inputs | Success and failure semantics |
| --- | --- | --- |
| NEGOTIATE | trace, capabilities | major=1, minor=0 and pinned_cpu required; responds pinned_cpu=true, cuda_ipc=false; repeated negotiation rejected |
| RESERVE | complete manifest | owns an unpublished reservation; registry and wire chunk limits enforced |
| PUT | owned reservation, chunk index, bytes, CRC32 | copies bytes after length/CRC checks; duplicate chunks rejected; failure leaves reservation retryable |
| COMMIT | owned reservation | verifies whole SHA256 and completeness, publishes index/registry object, returns a lease and source manifest |
| LOOKUP | query manifest, token IDs, exact flag or increasing prefix lengths | uses shared MatchIndex exact/longest-prefix and registry handles, returning a pinned lease |
| GET | owned lease, chunk index | copies source chunk bytes plus CRC32 into response; invalid or released lease rejected |
| RELEASE | owned lease | removes this pin only; never deletes the cached object; double release is NOT_FOUND |
| ABORT | owned reservation | drops unpublished chunks; double abort is NOT_FOUND |
| CANCEL | owned reservation | cancels a multi-message upload by aborting its reservation; cannot cancel another session's work |

Failed Commit retains reservation ownership and can be retried or aborted. Lease
metadata and ID are prepared before publishing. MatchIndex insertion is staged
before registry commit and rolled back without allocation on failure. A racing
lookup during staging can only miss, never return an unpublished object. After
successful registry commit, installing the preallocated lease slot cannot allocate.
Session-owned integration publication assumes the ordinary canonical registry key
function. Administrators evicting registry objects must also erase their index
entries; Release is not an eviction API. Stale index entries never yield a false
hit because the registry must confirm residency. This resident-only P8.1 path does
not claim disk loading via RequestPath.

Replies preserve operation and trace and contain stable schema status enums.
Transport framing errors return a local Status and require closing the connection.
A response encoding/allocation failure closes the session and cleans pending work
and pins. Like a lost network acknowledgement, it does not undo committed cache
publication; reconnecting callers can Lookup and reuse that object.

## Hit and recompute semantics

Prefill Lookup requires only `ValidateManifestIdentity`: token identity, tensor
geometry and compatibility metadata must be complete, but the tensor payload need
not have been computed. Its `payload_digest` may be absent or 32 zero bytes for an
unknown digest. Any supplied nonempty digest must be exactly 32 bytes; it is not a
matching constraint. A hit always returns the stored source manifest and its
verified, nonzero payload digest, not the query placeholder. Reserve still uses
full `ValidateManifest` and requires an exactly 32-byte, nonzero payload digest;
Commit verifies that digest against the uploaded bytes. Token digest validation
is unchanged and is mandatory for both queries and publications.

Token ranges are half-open [begin,end). A hit returns `[0,H)`, a lease of the
**source manifest**, its chunk indices and layer range. Partial hits return only
the absent suffix `[H,T)` in recompute_ranges. The source chunk bytes are not
repacked to the query shape; adapters interpret them using the returned manifest.
Full hits have H=T, no recompute ranges, and recompute=false. Exact lookup never
accepts a shorter match. The supplied token IDs/digest and prefix lengths are
validated by MatchIndex. Model, tenant, adapter and tensor compatibility partition
matching; incompatible queries do not hit.

A miss, budget failure, checksum failure, cancellation or deadline failure is
explicit, not a fabricated successful hit. Failed Lookup returns H=0, no lease or
hit ranges, recompute=true and `[0,T)` when T is nonzero. The adapter must recompute
those tokens and must never use payload from a failed response.

`deadline_unix_ms=0` means no deadline. Otherwise it is an absolute Unix millisecond
**admission deadline**, checked after acquiring the session serialization lock.
`cancelled=true` similarly rejects a request before execution. Synchronous bounded
resident operations are not interrupted mid-commit. CANCEL cancels an upload
handle between operations; it is not asynchronous preemption of a running GPU or
disk transfer. Late successful publication may be kept as reusable cache data;
adapters timing out abandon the request, release any acknowledged lease and
disconnect if acknowledgement is uncertain. Async disk/GPU cancellation belongs
to P8.2/P8.3 and their RequestPath integration.

## Adapter pinned memory ownership

`pinned_cpu` negotiates the adapter's CPU staging path, not page-locked storage in
the server. The adapter allocates and owns page-locked host memory using its
framework/CUDA allocator. Before serializing PUT it waits for the device-to-host
copy completion event. Protobuf serialization copies bytes into an owned message
and then an owned frame; after that copy completes the staging buffer may be
reused, independently of the server acknowledgement. The frame itself remains
owned by the transport until its send has completed. A failed PUT never transfers
ownership of any pointer or GPU allocation to the server.

On GET, the decoded response owns ordinary CPU bytes. The adapter verifies CRC32,
copies into its own pinned host buffer, and retains that pinned buffer until the
host-to-device completion event. It may release the server lease once all needed
chunks have been copied into adapter-owned storage; RELEASE does not free a
still-active adapter DMA buffer. Server registry data and response strings are
ordinary owned CPU memory, not mlock/CUDA-registered memory. No zero-copy, CUDA IPC,
GPU allocation, framework-private ABI or GPU performance claim is made by P8.1.
