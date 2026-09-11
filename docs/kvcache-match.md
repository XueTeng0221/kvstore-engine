# KVCache Match Index

## Query contract

The match index partitions entries by the complete tensor compatibility descriptor while excluding
token identity/length and physical payload/chunk details. The descriptor includes tenant, immutable
model/adapter/tokenizer revisions, cache-format ABI, layer range, dtype, validated layout and packing,
device, parallel topology, axis order, and every non-token/non-block dimension. Validation guarantees
that omitted token-dependent contiguous strides are derivable from the remaining descriptor.

Adapters provide the complete temporary token-ID sequence and strictly increasing reusable prefix
lengths. The index computes cumulative SHA-256 values in one pass and verifies the final digest
against the query manifest, so a caller cannot attach an existing prefix digest to an unrelated
query. Prefix lengths must be unique, nonzero, and no longer than the query. The index never logs or
stores raw tokens; the request-owned span only covers the synchronous lookup call.

Exact match is longest-prefix lookup constrained to the complete query digest. Longest-prefix lookup
examines candidate lengths in descending order, compares the cumulative token digest, and then
compares complete compatibility bytes after the partition digest match. SHA-256 supplies both the
prefix digest and partition key; complete compatibility comparison prevents a partition-hash
collision from becoming a cross-model or cross-tenant hit.

## Result and consistency

A hit returns the canonical object key, hit token count, layer range, all immutable object chunk
indices, and the missing half-open token range `[hit_tokens, query_tokens)`. Exact hits have no missing
range. P7.5 combines this logical result with a resident handle and tier state.

Insert, erase, and lookup are linearized by one shared mutex. Readers may run concurrently. A reader
racing erase observes either the complete entry before erase or `NOT_FOUND` after erase; no partial
entry is visible. Index insertion follows object publication, and erase precedes tier-store deletion
when integrated by P7.3/P7.5.

`kvcache_match_benchmark` independently crosses 128/1024/4096-token queries, 2/16/64 indexed prefix
objects, 1/4/16 concurrent readers, and partial-hit/miss outcomes. Each case performs a warmup lookup
and uses a start barrier. Production claims require repeated runs with percentile, CPU, and memory
capture under the full P7.4/P8 trace methodology.
