# CacheMeIfYouCan — Architecture & Workflow

A visual map of how a single client request flows through the server, and how
the code is organised into layers.

---

## 1. End-to-end request flow

```
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

```
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

```
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

```
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

```
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

| RESP | Meaning            | Example          |
|------|--------------------|------------------|
| `+`  | simple string      | `+PONG\r\n`      |
| `-`  | error              | `-ERR …\r\n`     |
| `:`  | integer            | `:42\r\n`        |
| `$N` | bulk string len N  | `$3\r\nbar\r\n`  |
| `$-1`| null bulk          | `$-1\r\n`        |
| `*N` | array of N items   | `*3\r\n…`        |

---

## 6. Commands implemented so far

| Command   | Arity        | Reply                            |
|-----------|--------------|----------------------------------|
| `PING`    | 0 or 1       | `+PONG` / bulk of arg            |
| `ECHO`    | 1            | bulk of arg                      |
| `SET k v` | 2 (+ PX ms)  | `+OK`                            |
| `GET k`   | 1            | bulk value or `$-1` (nil)        |
| `DEL k …` | ≥1           | integer (keys actually removed)  |
| `INCR k`  | 1            | incremented integer              |
| `KEYS p`  | 1            | array of matching keys           |
| `TYPE k`  | 1            | `string`, `list`, or `none`      |
| `CONFIG GET p` | 2       | config pair or empty array       |
| `RPUSH k v ...` | ≥2     | new list length                  |
| `LPUSH k v ...` | ≥2     | new list length                  |
| `LRANGE k s e` | 3       | array of list elements           |
| `LLEN k`  | 1            | list length                      |
| `LPOP k [count]` | 1-2   | popped value, array, or nil      |
| `BLPOP k ... timeout` | ≥2 | key/value pair or nil array    |

Dispatch is **case-insensitive** — both `PING` and `ping` route to `handlePing`.

---

## 7. Database as a keyspace facade

Redis commands are key-centric: the client says `DEL cart`, not
`delete cart from the string store`. Internally, however, the implementation
stores each data type in a separate container:

```
┌──────────────────────────────────────────────────────────────┐
│                          Database                            │
│                 central keyspace / facade                    │
└───────────────┬──────────────────┬───────────────────────────┘
                │                  │
                ▼                  ▼
       ┌────────────────┐  ┌────────────────┐
       │  StringStore   │  │   ListStore    │
       │  string keys   │  │   list keys    │
       └────────────────┘  └────────────────┘
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

```
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
  └──► FutureStore::del("cart")       // same pattern for new data types
  │
  ▼
return count of logical keys removed
```

This keeps command handlers small and prevents bugs where adding a new data
type requires updating `DEL`, `TYPE`, and `KEYS` in multiple places. When a new
store is added, the cross-type behavior is updated in `Database` once.

---

## 8. File map

```
include/
├── command/
│   ├── BasicCommands.hpp     # registerBasicCommands(Dispatcher&)
│   └── CommandDispatcher.hpp # Context, Dispatcher, toUpper()
├── protocol/
│   ├── RESPMessage.hpp       # the tagged union for parsed values
│   └── RESPParser.hpp        # RESPParser + encoders + parse_int
├── server/
│   ├── ClientSession.hpp     # handle_client(fd, ctx, disp)
│   └── TCPServer.hpp         # TCPServer(port, ctx, disp)
└── storage/
    ├── Database.hpp          # keyspace facade over typed stores
    ├── StringStore.hpp       # get / set / del / incr, expiry, mutex
    └── ListStore.hpp         # push / range / pop / blocking pop, mutex

src/
├── command/   { BasicCommands.cpp, ListCommands.cpp, CommandDispatcher.cpp }
├── protocol/  { RESPParser.cpp }
├── server/    { ClientSession.cpp, TCPServer.cpp }
├── storage/   { Database.cpp, StringStore.cpp, ListStore.cpp }
└── main.cpp
```
