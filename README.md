# CacheMeIfYouCan

A from-scratch, Redis-compatible in-memory data store written in modern C++.

![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![CMake](https://img.shields.io/badge/build-CMake-064F8C?logo=cmake&logoColor=white)
![Platform](https://img.shields.io/badge/platform-Linux-333333?logo=linux&logoColor=white)

CacheMeIfYouCan speaks the [RESP](https://redis.io/docs/reference/protocol-spec/)
wire protocol, so any Redis client — `redis-cli`, a language binding, or plain
`nc` — can talk to it. It is a learning project, not a production cache: the goal
is a clean, readable implementation of the ideas behind Redis (protocol framing,
a keyspace of typed stores, blocking commands, and transactions) rather than raw
throughput.

> **Status:** string/key, list, sorted-set, stream, geospatial, and pub/sub
> commands working over TCP, plus `MULTI`/`EXEC` transactions with optimistic
> `WATCH` locking, optional password authentication, and AOF persistence.
> Multi-threaded server, one thread per client.

---

## Architecture at a glance

```mermaid
classDiagram
    class TCPServer
    class TransactionManager
    class Dispatcher
    class Context
    class Database
    class StringStore
    class ListStore
    class SortedSetStore
    class StreamStore
    class PubSub
    class ClientState

    TCPServer --> Context : holds
    TCPServer --> Dispatcher : holds
    TCPServer ..> TransactionManager : one per client
    TCPServer ..> ClientState : one per client
    TransactionManager ..> Dispatcher : replays queue on EXEC
    Context --> Database : points to
    Context --> PubSub : points to
    Dispatcher ..> Context : passes to handlers
    Database *-- StringStore
    Database *-- ListStore
    Database *-- SortedSetStore
    Database *-- StreamStore
    PubSub ..> ClientState : delivers to
```

A per-client `TCPServer` thread runs the transaction manager and dispatcher; the
`Database` facade owns one typed store per data type, and the shared `PubSub`
registry fans messages out to each subscriber's `ClientState`. Full class
members and the dynamic flows (blocking `BLPOP`, `WATCH`/`EXEC`, `PUBLISH`
fan-out) are in [docs/uml.md](docs/uml.md).

---

## Features

- **RESP protocol** — full parser and encoder for simple strings, errors,
  integers, bulk strings, arrays, and null values, with correct handling of
  partial reads and pipelined commands.
- **Strings & keys** — `SET` (with `PX` expiry), `GET`, `DEL`, `INCR`, `KEYS`,
  `TYPE`, `CONFIG GET`.
- **Lists** — `RPUSH`, `LPUSH`, `LRANGE`, `LLEN`, `LPOP`, and blocking `BLPOP`.
- **Sorted sets** — `ZADD`, `ZRANK`, `ZRANGE`, `ZCARD`, `ZSCORE`, `ZREM`.
- **Streams** — `XADD` (auto, partial `ms-*`, and explicit IDs), `XRANGE`, and
  `XREAD` with `COUNT`, `BLOCK`, and the `$` cursor.
- **Geospatial** — `GEOADD`, `GEOPOS`, `GEODIST`, `GEOSEARCH`, sorted-set backed
  with a Redis-compatible 52-bit geohash (`TYPE` reports `zset`).
- **Pub/Sub** — `SUBSCRIBE`, `UNSUBSCRIBE`, `PUBLISH`, and `PUBSUB`
  (`CHANNELS`/`NUMSUB`), delivering messages across connections from a shared
  channel registry.
- **Transactions** — `MULTI`, `EXEC`, `DISCARD`, plus `WATCH`/`UNWATCH`
  optimistic locking that aborts the transaction if a watched key changed.
- **Authentication** — optional `--requirepass`; unauthenticated connections are
  rejected with `NOAUTH` until they send `AUTH`, plus `ACL WHOAMI`.
- **Persistence** — optional append-only file (`--appendonly yes`): every write is
  journaled as it runs and replayed at startup, with non-deterministic commands
  (`XADD *`, `BLPOP`, relative `PX` expiry) rewritten to their concrete effect.
- **Eviction** — optional `--maxkeys N` cap with exact LRU: a hand-rolled
  doubly-linked list tracks recency across every key type and evicts the
  least-recently-used key once the cap is passed.
- **Concurrency** — one thread per connection; each typed store guards its own
  data with a `std::mutex`, and blocking commands wait on a `condition_variable`.

No third-party libraries — just the C++20 standard library and POSIX sockets.

---

## Documentation

| Doc                                                  | What's in it                                                                                 |
| ---------------------------------------------------- | -------------------------------------------------------------------------------------------- |
| [docs/architecture.md](docs/architecture.md)         | Request flow, threading model, RESP framing, the keyspace facade, and the full command table |
| [docs/design-decisions.md](docs/design-decisions.md) | The choices worth explaining — and the tradeoffs each one accepts                            |
| [docs/uml.md](docs/uml.md)                           | Class and sequence diagrams (Mermaid, rendered inline on GitHub)                             |

---

## Building

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

For an optimized build:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Running

The server listens on port `6379` by default:

```bash
./redis_server
./redis_server --port 6380 --dir /tmp/redis-data --dbfilename dump.rdb
./redis_server --dir /tmp/redis-data --appendonly yes   # persist to an AOF
./redis_server --maxkeys 100000                         # LRU-evict beyond 100k keys
```

A quick smoke test without any client installed:

```bash
printf '*1\r\n$4\r\nPING\r\n' | nc -q 1 127.0.0.1 6379
# +PONG
```

## Usage

With `redis-cli` pointed at the server:

```text
$ redis-cli -p 6379

> SET greeting "hello"
OK
> GET greeting
"hello"
> RPUSH tasks a b c
(integer) 3
> LRANGE tasks 0 -1
1) "a"
2) "b"
3) "c"

> ZADD board 100 alice 80 bob
(integer) 2
> ZRANGE board 0 -1
1) "bob"
2) "alice"

> XADD events * kind login user 42
"1710000000000-0"
> XREAD COUNT 10 STREAMS events 0
...

# in one client:
> SUBSCRIBE news
1) "subscribe"
2) "news"
3) (integer) 1
# in another client:
> PUBLISH news "hello"
(integer) 1
# the first client then receives:
1) "message"
2) "news"
3) "hello"

> WATCH balance
OK
> MULTI
OK
> INCR balance
QUEUED
> EXEC          # aborts with (nil) if balance changed since WATCH
1) (integer) 1
```

---

## Testing

Two scripts under `tests/` exercise a real server over TCP — they boot it on a
scratch port and temp dir, drive it with `redis-cli`, and tear it down cleanly.

```bash
./tests/run_tests.sh     # integration checks; exits non-zero on any failure
./tests/run_bench.sh     # throughput via redis-benchmark
```

`run_tests.sh` runs ~50 assertions across every feature — strings, expiry,
lists, sorted sets, streams, geo, transactions, pub/sub, `WRONGTYPE`, auth
(`NOAUTH`/`WRONGPASS`/`AUTH`/`ACL WHOAMI`), and AOF surviving a restart. The
few things a serial script can't stage — cross-connection pub/sub delivery and a
`WATCH` abort race — stay manual.

### Benchmarks

`run_bench.sh` wraps `redis-benchmark` (optimized `Release` build). One request
at a time the server does ~16k ops/sec; pipelined — which the connection loop
drains natively — it reaches **~225k ops/sec** at depth 16, scaling past 400k at
higher depths:

| Mode                | SET   | GET   |
| ------------------- | ----: | ----: |
| serial (`-P 1`)     | ~16k  | ~16k  |
| pipelined (`-P 16`) | ~225k | ~229k |
| pipelined (`-P 32`) | ~331k | ~413k |
| pipelined (`-P 64`) | ~376k | ~503k |

Two changes unlock the pipelined numbers: `TCP_NODELAY` per connection (else
Nagle's algorithm stalls the batched replies) and coalescing a drained batch's
replies into a single `write()` instead of one per command. See
[design-decisions](docs/design-decisions.md) §16 for methodology and caveats.

---

## Author

[Shubh Garg](https://github.com/shubh-garg18)
