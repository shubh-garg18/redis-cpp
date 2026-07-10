# UML Diagrams

Diagrams are written in [Mermaid](https://mermaid.js.org/), which GitHub renders
inline — no image files to keep in sync. These cover what the ASCII diagrams in
[architecture.md](architecture.md) can't show well: the class structure and two
dynamic flows. The request flow, module layout, and RESP framing live in
architecture.md and are not repeated here.

---

## 1. Class diagram

The core types and how they relate. `♦──` is composition (owns), `──▷` is
inheritance/uses.

```mermaid
classDiagram
    class Context {
        +Database* db
        +string dir
        +string dbfilename
    }

    class Dispatcher {
        -unordered_map~string, CommandHandler~ handlers
        +add(cmd, fn) void
        +executeCommand(ctx, cmd, args) string
    }

    class TCPServer {
        -int port
        -Context* context
        -Dispatcher* dispatcher
        +start() void
    }

    class TransactionManager {
        -bool active
        -vector~QueuedCommand~ queue
        -unordered_map~string, uint64~ watched
        +shouldHandle(cmd) bool
        +handle(ctx, disp, cmd, args) string
    }

    class Database {
        +StringStore stringStore
        +ListStore listStore
        +SortedSetStore sortedSetStore
        +StreamStore streamStore
        -mutex versionMutex
        -unordered_map~string, uint64~ keyVersions
        +del(keys) int
        +typeOf(key) ValueType
        +keys(pattern) vector~string~
        +hasWrongType(key, expected) bool
        +version(key) uint64
        +touch(key) void
    }

    class StringStore {
        -mutex mutex
        +get(key) optional~string~
        +set(key, val, expiry) void
        +incr(key) int64
    }
    class ListStore {
        -mutex mutex
        -condition_variable cv
        +rpush(key, vals) int
        +lpop(keys, count) vector~string~
        +blpop(keys, timeout) optional~pair~
    }
    class SortedSetStore {
        -mutex mutex
        +zadd(key, entries) int
        +zrange(key, start, end) vector~string~
        +zrank(key, member) optional~int~
    }
    class StreamStore {
        -mutex mutex
        -condition_variable cv
        +xadd(key, ms, seq, fields) XAddResult
        +xrange(key, start, end) vector~StreamEntry~
        +xread(queries, block_ms) vector
        +lastId(key) StreamID
    }

    class StreamEntry {
        +StreamID id
        +vector~string~ fields
    }
    class StreamID {
        +uint64 ms
        +uint64 seq
    }
    class XAddResult {
        +bool ok
        +StreamID id
        +string error
    }

    TCPServer --> Context : holds
    TCPServer --> Dispatcher : holds
    TCPServer ..> TransactionManager : spawns per client
    TransactionManager ..> Dispatcher : replays queue on EXEC
    TransactionManager ..> Context : reads versions
    Context --> Database : points to
    Dispatcher ..> Context : passes to handlers

    Database *-- StringStore
    Database *-- ListStore
    Database *-- SortedSetStore
    Database *-- StreamStore

    StreamStore *-- StreamEntry
    StreamEntry *-- StreamID
    StreamStore ..> XAddResult : returns
```

---

## 2. Sequence: blocking `BLPOP` unblocked by another client

Two clients, one condition variable. The reader parks its thread; the writer
wakes it.

```mermaid
sequenceDiagram
    participant R as Client A (reader)
    participant HA as handle_client A
    participant L as ListStore
    participant HB as handle_client B
    participant W as Client B (writer)

    R->>HA: BLPOP q 0
    HA->>L: blpop([q], 0)
    Note over L: queue empty →<br/>cv.wait(lock)  (thread parked)
    W->>HB: RPUSH q hello
    HB->>L: rpush(q, [hello])
    L-->>HB: length 1
    Note over L: cv.notify_all()
    L-->>HA: (q, hello)
    HA->>R: 1) q  2) hello
    HB->>W: (integer) 1
```

---

## 3. Sequence: a transaction with `WATCH`

`WATCH` snapshots a version; `EXEC` compares it. If the watched key changed, the
whole transaction is discarded.

```mermaid
sequenceDiagram
    participant C as Client
    participant T as TransactionManager
    participant DB as Database
    participant D as Dispatcher

    C->>T: WATCH balance
    T->>DB: version("balance")
    DB-->>T: v = 7
    Note over T: watched["balance"] = 7
    C->>T: MULTI
    Note over T: active = true
    C->>T: INCR balance
    T-->>C: +QUEUED

    Note over DB: (another client SETs balance →<br/>touch bumps version to 8)

    C->>T: EXEC
    T->>DB: version("balance")
    DB-->>T: v = 8  ≠ 7
    Note over T: watch dirty → abort
    T-->>C: *-1  (nil, transaction aborted)
```
