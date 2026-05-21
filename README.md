# CacheMeIfYouCan

A from-scratch Redis-compatible in-memory data store, written in modern C++.

> **Status:** Core protocol + basic string commands working over TCP.
> Multi-threaded server, one thread per client.

---

## Building & running

```bash
mkdir build && cd build
cmake ..
make
./redis_server
```

The server listens on **port 6379**:

```bash
printf '*1\r\n$4\r\nPING\r\n' | nc -q 1 127.0.0.1 6379
# +PONG
```

Architecture, request flow, threading model and RESP framing are documented in
[docs/architecture.md](docs/architecture.md).

---

## Roadmap

### Core

- [x] TCP server + connection handling (one thread per client)
- [x] RESP protocol parser / encoder
- [x] Command dispatcher (case-insensitive)
- [x] `PING`, `ECHO`

### Strings & Keys

- [x] `SET` (with `PX` expiry), `GET`, `DEL`
- [ ] `INCR`
- [ ] `KEYS`, `TYPE`, `CONFIG GET`

### Data Structures

- [ ] Lists: `RPUSH`, `LPUSH`, `LRANGE`, `LLEN`, `LPOP`, `BLPOP`
- [ ] Sorted sets: `ZADD`, `ZRANK`, `ZRANGE`, `ZCARD`, `ZSCORE`, `ZREM`
- [ ] Streams: `XADD`, `XRANGE`, `XREAD` (incl. `BLOCK`)

### Server features

- [ ] Transactions: `MULTI`, `EXEC`, `DISCARD`
- [ ] Pub/Sub: `SUBSCRIBE`, `UNSUBSCRIBE`, `PUBLISH`
- [ ] Replication: `INFO`, `REPLCONF`, `PSYNC`, `WAIT`
- [ ] Geo: `GEOADD`, `GEOPOS`, `GEODIST`, `GEOSEARCH`
- [ ] RDB persistence

### Beyond

- [ ] AOF persistence
- [ ] Multi-database (`SELECT`)
- [ ] Eviction policies (LRU / LFU)
- [ ] Cluster sharding

---

## Author

[Shubh Garg](https://github.com/shubh-garg18)
