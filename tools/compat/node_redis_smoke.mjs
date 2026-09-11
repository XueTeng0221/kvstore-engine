import { createClient } from "redis";

const client = createClient({
  RESP: 2,
  socket: { host: "127.0.0.1", port: Number(process.argv[2]) },
});
await client.connect();
if ((await client.ping()) !== "PONG") throw new Error("PING failed");
await client.set("node:key", "1");
if ((await client.incr("node:key")) !== 2) throw new Error("INCR failed");
const values = await client.mGet(["node:key", "missing"]);
if (values[0] !== "2" || values[1] !== null) throw new Error("MGET failed");
if ((await client.del(["node:key", "missing"])) !== 1) throw new Error("DEL failed");
await client.close();
