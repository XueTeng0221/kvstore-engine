import sys

import redis


client = redis.Redis(host="127.0.0.1", port=int(sys.argv[1]), protocol=2, decode_responses=True)
assert client.ping()
assert client.set("py:key", "1")
assert client.incr("py:key") == 2
assert client.mget("py:key", "missing") == ["2", None]
assert client.delete("py:key", "missing") == 1
assert "kvstore_version" in client.info("server")
