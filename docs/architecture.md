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
6. The handler mutates `StringStore` (mutex-protected) and returns a
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
            │       Dispatcher  ──►  BasicCommands (PING …)    │
            └────────────────────┬─────────────────────────────┘
                                 │
                       ┌─────────┴──────────┐
                       ▼                    ▼
            ┌──────────────────┐  ┌──────────────────┐
            │    protocol/     │  │     storage/     │
            │  RESPParser +    │  │   StringStore    │
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
  reachable from every worker; only `StringStore` actually mutates and it
  guards itself with `std::mutex`.
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

Dispatch is **case-insensitive** — both `PING` and `ping` route to `handlePing`.

---

## 7. File map

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
    ├── Database.hpp          # struct Database { StringStore stringStore; }
    └── StringStore.hpp       # get / set / del, expiry, mutex

src/
├── command/   { BasicCommands.cpp, CommandDispatcher.cpp }
├── protocol/  { RESPParser.cpp }
├── server/    { ClientSession.cpp, TCPServer.cpp }
├── storage/   { StringStore.cpp }
└── main.cpp
```
