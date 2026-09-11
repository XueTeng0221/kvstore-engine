# KVCache Tiered Storage

## State and ownership

P7.3 adds `TieredStore` beside the P7.1 `ChunkRegistry`; it does not change P7.1 publication,
identity, or match semantics. Objects move through `resident`, `loading`, `evicting`, `disk-only`,
and `failed`. The public transition validator rejects direct disk-only to resident and resident to
loading changes. A resident hit returns the current shared pin without filesystem access.

Each resident chunk is copied into a 64-byte aligned rounded-capacity size class. Released allocations
remain in the pool's matching size class for reuse, and their capacity continues to count against
the hard physical budget; this prevents concurrent callers from exceeding the budget and makes
fragmentation include reusable capacity. Statistics report requested bytes, rounded bytes,
fragmentation, peak usage, allocation count, and high/low watermark state. Watermarks are
observations for P7.4 policy; the hard budget is enforced here. Handles own shared immutable
allocations. Eviction removes only the store's pins, and deletion hides and reclaims the disk
object; existing handles remain valid until their consumer releases them.

Move construction and move assignment deliberately keep the source handle valid and sharing the
same pin. This is stronger than the usual moved-from minimum and prevents adapters from accidentally
invalidating an in-flight tensor view.

The first caller promoting a disk-only object becomes the synchronous I/O leader. Other callers
wait on that object's shared state. Their `stop_token` or steady-clock deadline ends only their wait
and never cancels the leader or another consumer's required I/O. Capacity and transient I/O failures
leave the object disk-only for retry; integrity failures move it to failed. No detached worker is
created.

## Disk format and publication

Objects live below `objects/<first-two-sha256-hex>/<canonical-key-sha256-hex>/`. Chunk filenames are
the lowercase SHA-256 of their immutable content. Each chunk record in `object.meta` stores its
unsigned 64-bit byte length, CRC32, and SHA-256. Loads verify exact file length, reject trailing or
short data, verify CRC32 and chunk SHA-256, and finally verify the ordered payload SHA-256 from the
P7.1 manifest.

`object.meta` uses explicit big-endian fields: magic `KVD1`, format version `u16`, flags `u16`,
payload bytes `u64`, creation/access timestamps `u64`, chunk count `u32`, canonical-manifest length
`u32`, payload SHA-256, repeated chunk records, P7.1 canonical manifest bytes, and a final CRC32 over
all preceding metadata bytes. Lengths, count, configured object size, and integer arithmetic are
checked before allocation.

Publication writes every chunk to an exclusive temporary file with robust partial-write/EINTR
handling, fsyncs it, and atomically renames it. Metadata is written, fsynced, and renamed last, then
the object, shard, and `objects` root directories are fsynced; initial creation also fsyncs the store
directory and its parent. Therefore only an object with complete metadata is visible after restart.
Startup rejects malformed published metadata or missing chunk lengths and removes temporary files
and object directories that have no published metadata.

If metadata has been renamed but a later directory fsync fails, publication is an indeterminate
durable commit. The call returns `IO_ERROR`, retains a `failed` entry, and keeps the full disk charge;
it does not claim rollback or remove potentially recoverable data. An explicit delete can clean it,
and restart either discovers the complete metadata as disk-only or observes no object.

Disk quota includes chunk and metadata bytes. In-progress publications reserve quota under the
store mutex, preventing concurrent overcommit. `Delete` reclaims the object directory and fsyncs its
shard and the `objects` root before releasing quota. A partial removal failure marks the entry failed
rather than restoring a potentially stale resident state. Filesystem and allocation failures cross
the public boundary as `Status`/`Result`; payload bytes are never logged. Tests inject deterministic
ENOSPC-equivalent write, metadata rename/directory-sync, and delete-sync failures through an optional,
empty-by-default hook.

The configured store, `objects`, shard, and object directories must be real directories, not symbolic
links. File and directory opens use `O_NOFOLLOW` for their final component, and restart verifies that
every object resides under the shard matching the first two canonical digest characters. P9 extends
this local defense to deployment-wide path ownership and permissions.
