# Protocol and Command Contract v1

All integer fields are unsigned big-endian unless stated otherwise. Lengths are validated before allocation. The configured defaults are key <= 4 KiB, value <= 64 MiB, frame <= 68 MiB, and batch <= 1024 records. Keys and values are arbitrary bytes; empty keys are rejected and empty values are valid. TTL is not supported in v0.1.

## Native Text + KV framing

The native protocol is binary-safe despite its textual command line:

```text
KV/1 <COMMAND> <key-length> <value-length>\r\n
<key bytes><value bytes>
```

`value-length` is zero for commands without a value. `SAVE` and `LOAD` require both lengths to be zero. A response is:

```text
KV/1 <status-code> <payload-length>\r\n
<payload bytes>
```

Multiple frames may be pipelined. Parsers retain incomplete frames. A malformed header, arithmetic overflow, or frame over the configured limit produces `INVALID_ARGUMENT` or `LIMIT_EXCEEDED`; framing loss closes the connection after the error response.

## Batch v1 framing

```text
magic[4] = "KVB1"
version:u16 = 1
flags:u16 = 0
record_count:u32
payload_bytes:u64
records[payload_bytes]
```

Each record is `command:u8, key_length:u32, value_length:u64, key, value`. The response repeats the header and encodes each result as `status:u16, payload_length:u64, payload`. Records execute in input order and are individually atomic. Failure of one record does not roll back earlier records and does not skip later valid records.

## RESP

RESP compatibility is RESP2 in v0.1. Arrays of bulk strings map to typed commands. RESP3 negotiation is rejected as unsupported until P3. Native `SET` means create-only, while RESP `SET` maps to atomic `upsert`. RESP parsing and detailed compatibility are implemented in P3.

## Command semantics

| Command | Input | Success | Failure |
| --- | --- | --- | --- |
| `SET` | non-empty key, value | creates value | `ALREADY_EXISTS`, limits |
| `GET` | non-empty key | owned value bytes | `NOT_FOUND` |
| `DEL` | non-empty key | removes value | `NOT_FOUND` |
| `MOD` | non-empty key, value | replaces existing value | `NOT_FOUND`, limits |
| `EXIST` | non-empty key | boolean payload | only validation errors |
| `SAVE` | no key/value | durable snapshot accepted | I/O/status error |
| `LOAD` | no key/value | atomically installs valid snapshot | busy, corrupt, incompatible, I/O error |

Engine-only `UPSERT` and `INCREMENT` primitives support RESP. `INCREMENT` accepts canonical base-10 signed 64-bit bytes, rejects whitespace, plus signs, leading zeroes except `0`, negative zero, non-digits, and overflow. Missing keys begin at zero. Delta addition is checked before mutation.

## Stable status codes

| Code | Name | Meaning |
| ---: | --- | --- |
| 0 | `OK` | operation succeeded |
| 1 | `NOT_FOUND` | key or resource absent |
| 2 | `ALREADY_EXISTS` | create-only conflict |
| 3 | `INVALID_ARGUMENT` | malformed command or integer |
| 4 | `LIMIT_EXCEEDED` | configured size/capacity exceeded |
| 5 | `IO_ERROR` | durable storage operation failed |
| 6 | `CORRUPTION` | checksum or format validation failed |
| 7 | `UNSUPPORTED` | protocol/version/operation unavailable |
| 8 | `CANCELLED` | caller cancellation observed before commit |
| 9 | `READ_ONLY` | mutation sent to replica |
| 10 | `BUSY` | bounded queue/state prevents admission |
| 11 | `INTERNAL` | invariant or unexpected internal failure |

Status names and numeric values are stable within protocol v1. Errors never include complete keys, values, credentials, or raw KV tensors.
