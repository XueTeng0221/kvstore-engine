# KVCache Format Version 1

## Identity and compatibility

KVCache objects are immutable after publication. The canonical cache key is
`kvc1:<sha256>`, where SHA-256 covers the versioned, big-endian canonical manifest. Every
variable-length field is encoded with a 32-bit byte length. The encoding does not dump C++ object
memory and is shared with the P8 integration schema through explicit field mapping.

The key covers tenant, model ID and revision, adapter ID and revision, tokenizer revision, token
digest and count, layer range, dtype, shape, axis order, byte strides, tensor layout, K/V packing,
block size, framework cache-format ABI, source device, tensor/pipeline parallel topology, payload
size, chunking, and compression. Creation/access timestamps and payload checksum do not select
compatibility and are excluded from the key. A lookup receives the expected manifest and compares
its complete canonical bytes after the digest match; a digest collision is reported as corruption
rather than treated as a hit. Lookup callers leave payload checksum zero because content is not
yet known; reserve/commit require it and commit verifies the ordered bytes. Version 1 accepts only
contiguous tensors and float16, bfloat16, or float32; quantized KV needs a later manifest contract
that represents scales explicitly.

Changing a model revision, tokenizer revision, adapter identity, dtype, shape, layout, layer range,
parallel topology, or token digest creates a different key. Deployments must use immutable model
revisions. An adapter is either absent (both adapter fields empty) or fully identified. In-place
model, adapter, or tokenizer upgrades under the same revision are unsupported because they cannot
be invalidated safely.

## Canonical wire order

Unsigned integers are big-endian. Strings are binary UTF-8 bytes prefixed by a big-endian `u32`
length. Digests are exactly 32 bytes. Token digest input is the ordered token IDs encoded as
big-endian `u32` values; adapters must reject token IDs outside that range.

Fields follow this exact order: magic `KVC1`, manifest version `u16`, tenant, model ID, model
revision, adapter ID, adapter revision, tokenizer revision, cache format, cache format version,
token digest, token count, layer begin, layer count, dtype, rank, then for every dimension its axis,
extent, and contiguous byte stride; tensor layout, K/V packing, block tokens, device kind and index,
tensor-parallel rank and size, pipeline-parallel rank and size, payload bytes, chunk bytes, chunk
alignment, chunk count, and compression.

Enum wire values are stable within version 1: dtype `float16=1`, `bfloat16=2`, `float32=3`; layout
`layer-major=1`, `block-major=2`; axis `key-value=1`, `layer=2`, `token=3`, `block=4`, `head=5`,
`head-dimension=6`; K/V packing `planar=1`, `interleaved=2`; device `cpu=1`, `cuda=2`; compression
`none=0`. Unknown values are rejected. The unit-test manifest has golden key
`kvc1:42289701a2b511cf95219d0fc7d1501101a7f49b8f1e3a2fe14b6146f71e9372`.

Version 1 recognizes exactly three contiguous axis layouts. Layer-major planar is
`[key-value, layer, token, head, head-dimension]`. Block-major planar is
`[layer, key-value, block, token, head, head-dimension]`. Block-major interleaved is
`[layer, block, token, key-value, head, head-dimension]`. Other layout/packing/axis combinations
are contradictory descriptors and are rejected.

## Chunks and publication

Manifest version 1 supports only uncompressed chunks. Chunk starts use 64-byte logical alignment;
`chunk_bytes` is a multiple of 64 in `(0, 64 MiB]`, and `chunk_count` must equal
`ceil(payload_bytes / chunk_bytes)`. P7.3's resident pool must provide the same physical alignment.
Each chunk carries CRC32 for fast corruption detection; the complete ordered payload is verified
against SHA-256 before publication. The shape product multiplied by dtype width must exactly equal
`payload_bytes`.

Publication uses `reserve -> put chunks -> commit`. Reservations and partial chunks are invisible
to lookup. Commit atomically associates the immutable manifest with all chunks only after count,
length, CRC32, and payload SHA-256 checks pass. Abort drops all unpublished chunks. Delete removes
the object from future lookup atomically; existing resident handles retain shared read-only ownership
until inference consumers release them.

The registry enforces hard count and logical object-size limits before allocating the chunk table.
Its tracked-byte admission estimate includes payload, two copies of canonical-sized dynamic
manifest data, chunk bookkeeping plus allocator allowance, and fixed object/index overhead. Deleted
objects retained by resident handles remain charged. This estimate is intentionally conservative but
is not an allocator-level RSS guarantee; P7.3 supplies the bounded slab/pool required for a physical
memory budget and timeout-based reservation cleanup. Callers must abort abandoned P7.1 reservations
explicitly.

P7.3 stores the same immutable chunk representation in directory-sharded, content-addressed files
published with temporary-file fsync and rename. Compression requires a new recognized enum and
format behavior; version 1 rejects unknown compression values.

## Dependency

SHA-256 uses PicoSHA2 1.0.1, pinned by archive hash in CMake. PicoSHA2 is MIT licensed and was chosen
as a small, portable implementation because the project does not otherwise require a system crypto
library. SHA-256 here provides identity and collision resistance, not authentication.
