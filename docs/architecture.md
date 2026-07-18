# CacheMeIfYouCan — Architecture & Workflow

A visual map of how a single client request flows through the server, and how
the code is organised into layers.

---

## 1. End-to-end request flow

```text
┌────────────┐                                                      ┌────────────┐
│  Client    │   1. TCP connect 127.0.0.1:6379                      │            │
│ (redis-cli)│ ───────────────────────────────────────────────────► │ TCPServer  │
│            │                                                      │  (accept)  │
│            │   2. *3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n   │            │
│            │ ───────────────────────────────────────────────────► │            │
└────────────┘                                                      └─────┬──────┘
       ▲                                                                  │
       │                                                                  │ 3. spawn thread
       │                                                                  ▼
       │                                                            ┌────────────┐
       │                                                            │handle_client│
       │   6. +OK\r\n                                               │  (per-conn) │
       │ ◄────────────────────────────────────────────────────────  └─────┬──────┘
       │                                                                  │
       │                                                                  │ 4. parse RESP
       │                                                                  ▼
       │                                                            ┌────────────┐
       │                                                            │ RESPParser │
       │                                                            │  (frames)  │
       │                                                            └─────┬──────┘
       │                                                                  │
       │                                                                  │ 5. dispatch
       │                                                                  ▼
       │                                                            ┌────────────┐
       │                                                            │ Dispatcher │
       │                                                            │ (handleSet)│
       │                                                            └─────┬──────┘
       │                                                                  │
       │                                                                  │ 6. set + reply
       │                                                                  ▼
       │                                                            ┌────────────┐
       └──────────────────────────────────────────────────────────► │StringStore │
                                                                    │  (mutex)   │
                                                                    └────────────┘
```

**Steps:**

1. Client opens a TCP connection to `0.0.0.0:6379`.
2. Sends a RESP-encoded array of bulk strings.
3. `TCPServer::start()` accepts the connection and spawns a worker thread
   running `handle_client(fd, &context, dispatcher)`.
4. `handle_client` reads bytes, buffers them, and asks `RESPParser` to extract
   one complete frame.
5. The frame's first bulk string is the command name; the rest are arguments.
   `Dispatcher::executeCommand` looks up the handler and runs it.
6. The handler mutates the relevant store (mutex-protected) and returns a
   RESP-encoded reply, which is written back to the socket.

---

## 2. Module layout

```text
            ┌──────────────────────────────────────────────────┐
            │                     main.cpp                     │
            │     (wires Database + Dispatcher + TCPServer)    │
            └────────────────────┬─────────────────────────────┘
                                 │
                                 ▼
            ┌──────────────────────────────────────────────────┐
            │                    server/                       │
            │   TCPServer  ──spawns──►  handle_client (thread) │
            └────────────────────┬─────────────────────────────┘
                                 │
                                 ▼
            ┌──────────────────────────────────────────────────┐
            │                   command/                       │
            │       Dispatcher  ──►  Basic/List commands       │
            └────────────────────┬─────────────────────────────┘
                                 │
                       ┌─────────┴──────────┐
                       ▼                    ▼
            ┌──────────────────┐  ┌──────────────────┐
            │    protocol/     │  │     storage/     │
            │  RESPParser +    │  │    Database      │
            │  encoders        │  │   (mutex map)    │
            └──────────────────┘  └──────────────────┘
```

Dependencies flow **downward only** — `server` depends on `command`,
`command` depends on `protocol` and `storage`. Lower layers know nothing
about the layers above.

---

## 3. Concurrency model

```text
                       main thread
                  ┌──────────────────┐
                  │ TCPServer::start │
                  │  (in accept())   │
                  └────────┬─────────┘
                           │  accept() returns new fd
              ┌────────────┼────────────┐
              ▼            ▼            ▼
        ┌──────────┐ ┌──────────┐ ┌──────────┐
        │ worker 1 │ │ worker 2 │ │ worker N │
        │  client  │ │  client  │ │  client  │
        │  thread  │ │  thread  │ │  thread  │
        └─────┬────┘ └─────┬────┘ └─────┬────┘
              │            │            │
              └────────────┼────────────┘
                           ▼
                  ┌──────────────────┐
                  │   StringStore    │
                  │   std::mutex     │  ◄── serialises all set/get/del
                  └──────────────────┘
```

- **One thread per client.** Simple, OS-scheduled, costs ~MB of stack per
  thread. Scales to hundreds, breaks down around the C10K mark.
- **Shared state.** `Database`, `Dispatcher`, and the command table are
  reachable from every worker; typed stores own mutable data and guard it with
  `std::mutex`.
- **No graceful shutdown / no connection cap** — deliberate omissions for
  scope.

---

## 4. Per-connection state machine

```text
                      ┌─────────────────────┐
                      │   handle_client     │
                      │   (per-thread)      │
                      └──────────┬──────────┘
                                 │
                                 ▼
                      ┌─────────────────────┐
                ┌────►│  read() into chunk  │
                │     └──────────┬──────────┘
                │                │ append to buffer
                │                ▼
                │     ┌─────────────────────┐
                │     │ RESPParser(buffer)  │
                │     └──────────┬──────────┘
                │                │
                │      ┌─────────┼──────────┐
                │      ▼         ▼          ▼
                │ incomplete    ok        throws
                │  (need        │            │
                │   more) ──────┘            │
                │                            ▼
                │     ┌─────────────────────┐
                │     │ Dispatcher.execute  │
                │     └──────────┬──────────┘
                │                │
                │                ▼
                │     ┌─────────────────────┐
                │     │  write_all(reply)   │
                │     └──────────┬──────────┘
                │                │
                │                ▼
                │     ┌─────────────────────┐
                │     │ erase consumed bytes│
                │     │ from buffer         │
                │     └──────────┬──────────┘
                │                │
                │      ┌─────────┴──────────┐
                │      ▼                    ▼
                │ buffer empty         more frames left?
                │  ──► read again ─────┘   (drain inner loop)
                │
        EOF / error / parse throw
                ▼
           close(fd)
```

The **outer loop = read syscalls**; the **inner loop drains every complete
frame already in the buffer** before going back to read. This handles
pipelined commands (multiple commands in one TCP segment) and partial reads
(half a command split across two segments).

---

## 5. RESP framing (the bytes on the wire)

```text
SET foo bar
↓
*3\r\n            ← array of 3 elements
$3\r\nSET\r\n     ← bulk string of length 3
$3\r\nfoo\r\n
$3\r\nbar\r\n

reply:
+OK\r\n           ← simple string
```

Other reply types:

| RESP  | Meaning           | Example         |
| ----- | ----------------- | --------------- |
| `+`   | simple string     | `+PONG\r\n`     |
| `-`   | error             | `-ERR …\r\n`    |
| `:`   | integer           | `:42\r\n`       |
| `$N`  | bulk string len N | `$3\r\nbar\r\n` |
| `$-1` | null bulk         | `$-1\r\n`       |
| `*N`  | array of N items  | `*3\r\n…`       |

---

## 6. Commands implemented so far

| Command                                                      | Arity       | Reply                                             |
| ------------------------------------------------------------ | ----------- | ------------------------------------------------- |
| `PING`                                                       | 0 or 1      | `+PONG` / bulk of arg                             |
| `ECHO`                                                       | 1           | bulk of arg                                       |
| `SET k v`                                                    | 2 (+ PX ms) | `+OK`                                             |
| `GET k`                                                      | 1           | bulk value or `$-1` (nil)                         |
| `DEL k …`                                                    | ≥1          | integer (keys actually removed)                   |
| `INCR k`                                                     | 1           | incremented integer                               |
| `KEYS p`                                                     | 1           | array of matching keys                            |
| `TYPE k`                                                     | 1           | `string`, `list`, `zset`, `stream`, or `none`     |
| `CONFIG GET p`                                               | 2           | config pair or empty array                        |
| `RPUSH k v ...`                                              | ≥2          | new list length                                   |
| `LPUSH k v ...`                                              | ≥2          | new list length                                   |
| `LRANGE k s e`                                               | 3           | array of list elements                            |
| `LLEN k`                                                     | 1           | list length                                       |
| `LPOP k [count]`                                             | 1-2         | popped value, array, or nil                       |
| `BLPOP k ... timeout`                                        | ≥2          | key/value pair or nil array                       |
| `ZADD k s m ...`                                             | ≥3          | new members added                                 |
| `ZRANK k m`                                                  | 2           | rank integer or nil                               |
| `ZRANGE k s e`                                               | 3           | array of members                                  |
| `ZCARD k`                                                    | 1           | member count                                      |
| `ZSCORE k m`                                                 | 2           | bulk score or nil                                 |
| `ZREM k m ...`                                               | ≥2          | members removed                                   |
| `XADD k id f v ...`                                          | ≥4          | bulk of the assigned id                           |
| `XRANGE k s e`                                               | 3           | array of `[id, [f, v, ...]]`                      |
| `XREAD [COUNT n] [BLOCK ms] STREAMS k ... id ...`            | ≥3          | per-stream entries or nil array                   |
| `GEOADD k lon lat m ...`                                     | ≥4          | new members added                                 |
| `GEOPOS k m ...`                                             | ≥1          | array of `[lon, lat]` (or nil per missing member) |
| `GEODIST k m1 m2 [unit]`                                     | 3-4         | bulk distance or nil                              |
| `GEOSEARCH k FROMLONLAT lon lat BYRADIUS r unit [ASC\|DESC]` | ≥7          | array of members in range                         |
| `SUBSCRIBE ch ...`                                           | ≥1          | one `[subscribe, ch, count]` array per channel    |
| `UNSUBSCRIBE [ch ...]`                                       | ≥0          | one `[unsubscribe, ch, count]` array per channel  |
| `PUBLISH ch msg`                                             | 2           | integer (subscribers the message reached)         |
| `PUBSUB CHANNELS` / `PUBSUB NUMSUB ch ...`                   | ≥1          | active channels / flat `[ch, count, ...]` array   |
| `AUTH [user] password`                                       | 1-2         | `+OK` or `WRONGPASS` / `NOAUTH` error             |
| `ACL WHOAMI`                                                 | 1           | bulk `default`                                    |

Dispatch is **case-insensitive** — both `PING` and `ping` route to `handlePing`.

`MULTI`, `EXEC`, `DISCARD`, `WATCH`, and `UNWATCH` never reach the dispatcher —
`TransactionManager` intercepts them per connection (see §9).

---

## 7. Database as a keyspace facade

Redis commands are key-centric: the client says `DEL cart`, not
`delete cart from the string store`. Internally, however, the implementation
stores each data type in a separate container:

```text
┌──────────────────────────────────────────────────────────────┐
│                          Database                            │
│                 central keyspace / facade                    │
└───────────────┬──────────────────┬───────────────────────────┘
        ┌───────┬───────┴───────┬───────┐
        ▼       ▼               ▼       ▼
┌────────────┐┌────────────┐┌────────────┐┌────────────┐
│StringStore ││ ListStore  ││SortedSet   ││StreamStore │
│string keys ││ list keys  ││Store zsets ││stream keys │
└────────────┘└────────────┘└────────────┘└────────────┘
```

The `Database` facade owns cross-type behavior:

- `DEL key ...` removes keys no matter which typed store currently owns them.
- `TYPE key` asks the keyspace which kind of value the key holds.
- `KEYS pattern` combines keys from all typed stores.
- Wrong-type checks stay consistent when commands operate on a specific type.

Each store still keeps its own low-level delete function because only that
store knows its private data structure and locking rules. `Database` does not
reach into private maps directly; it coordinates the stores through their
public APIs.

`DEL` flow:

```text
Client
  │
  │  DEL cart
  ▼
ClientSession
  │  parse RESP frame
  ▼
Dispatcher
  │  route to handleDel(...)
  ▼
BasicCommands::handleDel
  │  collect key names
  ▼
Database::del({"cart"})
  │
  ├──► StringStore::del("cart")       // erase if it is a string key
  │
  ├──► ListStore::del("cart")         // erase if it is a list key
  │
  ├──► SortedSetStore::del("cart")    // erase if it is a sorted-set key
  │
  └──► StreamStore::del("cart")       // erase if it is a stream key
  │
  ▼
return count of logical keys removed
```

This keeps command handlers small and prevents bugs where adding a new data
type requires updating `DEL`, `TYPE`, and `KEYS` in multiple places. When a new
store is added, the cross-type behavior is updated in `Database` once.

---

## 8. File map

```text
include/
├── command/
│   ├── BasicCommands.hpp     # registerBasicCommands(Dispatcher&)
│   ├── ListCommands.hpp      # registerListCommands(Dispatcher&)
│   ├── SortedSetCommands.hpp # registerSortedSetCommands(Dispatcher&)
│   ├── StreamCommands.hpp    # registerStreamCommands(Dispatcher&)
│   ├── GeoCommands.hpp       # registerGeoCommands(Dispatcher&)
│   ├── PubSubCommands.hpp    # registerPubSubCommands(Dispatcher&)
│   ├── AuthCommands.hpp      # registerAuthCommands(Dispatcher&)
│   └── CommandDispatcher.hpp # Context, Dispatcher, toUpper()
├── auth/
│   └── AuthConfig.hpp        # server-wide requirepass (header-only)
├── protocol/
│   ├── RESPMessage.hpp       # the tagged union for parsed values
│   └── RESPParser.hpp        # RESPParser + encoders + parse_int
├── persistence/
│   └── AofWriter.hpp         # append-only file writer (thread-safe)
├── pubsub/
│   └── PubSub.hpp            # channel → subscribers registry
├── server/
│   ├── ClientSession.hpp     # handle_client(fd, ctx, disp)
│   ├── ClientState.hpp       # per-connection fd + writeMutex + channels + auth
│   ├── TransactionManager.hpp# per-conn MULTI/EXEC/WATCH state
│   └── TCPServer.hpp         # TCPServer(port, ctx, disp)
└── storage/
    ├── Database.hpp          # keyspace facade over typed stores
    ├── StringStore.hpp       # get / set / del / incr, expiry, mutex
    ├── ListStore.hpp         # push / range / pop / blocking pop, mutex
    ├── SortedSetStore.hpp    # zadd / zrank / zrange ..., map + sorted set
    └── StreamStore.hpp       # xadd / xrange / xread, append-only, mutex + cv

src/
├── command/   { BasicCommands.cpp, ListCommands.cpp, SortedSetCommands.cpp,
│                StreamCommands.cpp, GeoCommands.cpp, PubSubCommands.cpp,
│                AuthCommands.cpp, CommandDispatcher.cpp }
├── persistence/ { AofWriter.cpp }
├── protocol/  { RESPParser.cpp }
├── pubsub/    { PubSub.cpp }
├── server/    { ClientSession.cpp, TCPServer.cpp, TransactionManager.cpp }
├── storage/   { Database.cpp, StringStore.cpp, ListStore.cpp,
│                SortedSetStore.cpp, StreamStore.cpp }
└── main.cpp
```

---

## 9. Transactions (`MULTI` / `EXEC` / `WATCH`)

`TransactionManager` is a **per-connection** object living in `handle_client`.
It sees every command before the dispatcher does: `shouldHandle` returns true
for `MULTI`/`EXEC`/`DISCARD`/`WATCH`/`UNWATCH`, and for _any_ command once a
`MULTI` is open (so those get queued instead of run).

```text
WATCH k          record k's current version
MULTI            open the queue
SET k v          ─┐ commands are queued, each replies +QUEUED
INCR n           ─┘
EXEC             if any watched key's version moved → abort, reply *-1
                 else run the queue in order, reply an array of results
```

Optimistic locking rides on `Database::version(key)` / `touch(key)` — a
monotonic per-key counter. Every mutating handler (`SET`, `DEL`, `INCR`, the
list/zset/stream writers) calls `touch`, so `EXEC` can detect whether a watched
key changed between `WATCH` and `EXEC` and abort the whole transaction.

---

## 10. Pub/Sub (cross-connection delivery)

Every command until now replied only to the client that sent it. `PUBLISH`
breaks that: it runs on the publisher's thread but has to write to _other_
connections' sockets. Three pieces make that possible.

- **`ClientState`** — a per-connection object (socket `fd`, a `writeMutex`, and
  the set of channels it is subscribed to). It gives a connection an identity
  that other threads can reach.
- **Per-connection `Context` copy** — `handle_client` copies the shared
  `Context` and stamps its own `ClientState*` into it, so a handler can answer
  "who am I?" (`ctx.client`) while still sharing one `Database` and one
  `PubSub`.
- **`PubSub`** — a shared registry mapping `channel → {ClientState*}`, guarded
  by one mutex. `SUBSCRIBE` registers the caller; `PUBLISH` looks up the channel
  and writes the `message` frame into each subscriber's `fd`.

```text
Client A                      Client B
  │ SUBSCRIBE news              │
  ▼                            │
handle_client A                │
  │ pubsub.subscribe(A,news)   │
  │  channelSubs[news] += A    │
  ▼                            ▼
(blocked in read())      handle_client B
                              │ PUBLISH news "hi"
                              ▼
                          pubsub.publish(news,"hi")
                              │  for each sub: write(sub.fd, frame)
                              ▼   (locks A.writeMutex)
                          A's socket ◄── *3 message news hi
                              │
                              ▼  reply to B
                          :1  (one subscriber reached)
```

Two threads can now write to the same socket — a `PUBLISH` from another thread
and the subscriber's own replies — so **every** write goes through that
connection's `writeMutex`. Lock order is always registry → `writeMutex`, never
the reverse, so there is no deadlock. On disconnect the connection removes
itself from the registry (guarded so it runs even if a handler throws) before
its `ClientState` is destroyed, so `PUBLISH` never touches a freed pointer.

Delivery holds the registry mutex while writing to sockets — simple and safe,
but a stalled subscriber can back-pressure the subsystem. Real Redis uses
non-blocking sockets with per-client output buffers; that is the deliberate
tradeoff here.

---

## 11. Authentication (the `NOAUTH` gate)

If the server is started with `--requirepass <pw>`, connections must
authenticate before they can do anything. Two pieces of state, at two scopes:

- **`AuthConfig`** — one server-wide struct holding `requirepass`. An empty
  string means auth is disabled (`enabled()` is `false`), so there is no
  separate on/off flag.
- **`ClientState::authenticated`** — a per-connection `bool`, false until this
  connection sends a correct `AUTH`.

The check is a gate in the connection loop, placed **before** the transaction
manager and the dispatcher:

```text
frame arrives
   │
   ▼
auth enabled? ──no──►  (normal dispatch, unchanged)
   │ yes
   ▼
authenticated?  ──yes──►  (normal dispatch)
   │ no
   ▼
command == AUTH? ──yes──►  handleAuth  (may set authenticated = true)
   │ no
   ▼
-NOAUTH Authentication required.
```

Putting the gate first is what makes it airtight: `MULTI`, `WATCH`, `SUBSCRIBE`,
and every other command hit the `NOAUTH` branch while unauthenticated, so an
unauthenticated client can't even open a transaction to smuggle commands past
the check. `AUTH` is the single command allowed through, and the exemption is
case-insensitive (`toUpper(cmd) != "AUTH"`). When no password is set the whole
gate short-circuits and behaviour is identical to before.

---

## 12. AOF persistence (journal + replay)

With `--appendonly yes`, the server survives a restart by keeping a log of every
write. It's a write-ahead journal: each mutating command is appended to the file
as a RESP array as it runs, and on startup the file is replayed to rebuild the
dataset.

**The hook lives in `Dispatcher::executeCommand`, not the connection loop.**
After a handler runs, `propagateToAof` appends the command if it's a write. That
location is deliberate: `EXEC` replays queued commands _through the dispatcher_,
so hooking here captures transaction writes for free — hooking in the connection
loop would miss them.

```text
executeCommand(cmd)
   │
   ▼
run handler ──► reply
   │
   ▼
propagateToAof(cmd, args, reply)
   │  write command?  errored?  aof open?
   ▼
append RESP to the AOF file (+ flush)
```

**Non-deterministic commands are rewritten to what actually happened**, so replay
reproduces the same state rather than re-rolling the dice:

| Command      | Journaled as                       | Why                                                  |
| ------------ | ---------------------------------- | ---------------------------------------------------- |
| `XADD k *`   | `XADD k <concrete-id>`             | `*` would generate a _new_ id on replay              |
| `BLPOP k t`  | `LPOP <popped-key>`                | a blocking pop replays as a plain pop; timeouts skip |
| `SET k v PX` | `SET k v PXAT <absolute-deadline>` | relative TTL would re-base to the restart clock      |

**Replay happens before the writer opens.** `replayAOF` runs the file through
`executeCommand` while `ctx.aof` is still null, so the `isOpen()` guard in
`propagateToAof` is false and replayed writes are _not_ re-appended into the file
being read. Only after replay does `main` `open()` the writer and set `ctx.aof`.
A truncated or corrupt tail (from a crash mid-write) stops replay cleanly rather
than failing.

**Known limitations** (deliberate, for a thread-per-client learning build):

- The append happens _after_ the handler's mutation, under a separate mutex, so
  two concurrent writes to the same key can be journaled in the opposite order
  they were applied. A global write lock would fix it but would undo the
  one-mutex-per-store design.
- `fflush` (not `fsync`) after each write: survives a process crash, not an OS
  crash / power loss.
- Expiry uses a monotonic clock, so `PXAT` deadlines are correct across a process
  restart (the case AOF protects) but not across a machine reboot.
