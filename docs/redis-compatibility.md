# Redis Compatibility

KVStore v0.1 accepts RESP2 command arrays and has been smoke-tested with redis-py 6.x,
Node redis 6.x, go-redis 9.7 in explicit RESP2 mode, and redis-cli 6.0.16. The
redis-cli CRUD, stdin pipeline, and `INFO server` paths pass. redis-cli `--pipe` mode is
not supported because its transfer probe includes framing outside the v0.1 array-only command
contract.

Supported commands are `SET`, `GET`, `DEL`, `EXISTS`, `MGET`, `INCR`, `DECR`,
`PING`, `ECHO`, `CLIENT SETINFO`, `CLIENT SETNAME`, `CLIENT GETNAME`, `INFO`, and
`SAVE`. `SET` uses Redis overwrite semantics. `SAVE` is synchronous. `LOAD` is not
exposed through RESP because online AOF boundary replacement is unsupported.

RESP3 negotiation (`HELLO 3`), authentication, TTL commands, transactions, scripts,
pub/sub, streams, cluster commands, and Redis persistence commands other than `SAVE`
are unsupported. Unsupported commands return an error without closing a healthy RESP2
connection. Malformed framing returns an error and closes the connection.

The smoke clients live under `tools/compat/`. They require a running server and take the
server port as their first argument.
