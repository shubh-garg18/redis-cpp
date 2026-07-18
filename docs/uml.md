# UML Diagrams

Diagrams are written in [Mermaid](https://mermaid.js.org/), which GitHub renders
inline — no image files to keep in sync. These cover what the ASCII diagrams in
[architecture.md](architecture.md) can't show well: the class structure and
three dynamic flows. The request flow, module layout, and RESP framing live in
architecture.md and are not repeated here.

---

## 1. Class diagram

The core types and how they relate. `♦──` is composition (owns), `..>` is
uses/depends. Command modules (`<<module>>`) are groups of free handler
functions — each registers on the `Dispatcher` and operates on one store; note
`GeoCommands` has no store of its own, storing its geohash in the sorted set,
`PubSubCommands` operates on the shared `PubSub` registry (reaching other
connections' `ClientState`) rather than on `Database`, and `AuthCommands` reads
the shared `AuthConfig` and flips its own connection's `authenticated` flag.

```mermaid
classDiagram
    class Context {
        +Database* db
        +string dir
        +string dbfilename
        +ClientState* client
        +PubSub* pubsub
        +AuthConfig* auth
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

    class PubSub {
        -mutex mutex
        -unordered_map~string, set~ClientState*~~ channelSubs
        +subscribe(c, channel) void
        +unsubscribe(c, channel) void
        +dropClient(c) void
        +publish(channel, message) int
        +listChannels() vector~string~
        +numSubscribers(channel) int
    }
    class ClientState {
        +int fd
        +mutex writeMutex
        +unordered_set~string~ channels
        +bool authenticated
        +inSubscribeMode() bool
        +subscriptionCount() size_t
    }
    class AuthConfig {
        +string requirepass
        +enabled() bool
    }

    class BasicCommands {
        <<module>>
        +registerBasicCommands(Dispatcher&)
    }
    class ListCommands {
        <<module>>
        +registerListCommands(Dispatcher&)
    }
    class SortedSetCommands {
        <<module>>
        +registerSortedSetCommands(Dispatcher&)
    }
    class StreamCommands {
        <<module>>
        +registerStreamCommands(Dispatcher&)
    }
    class GeoCommands {
        <<module>>
        +registerGeoCommands(Dispatcher&)
    }
    class PubSubCommands {
        <<module>>
        +registerPubSubCommands(Dispatcher&)
    }
    class AuthCommands {
        <<module>>
        +registerAuthCommands(Dispatcher&)
    }

    TCPServer --> Context : holds
    TCPServer --> Dispatcher : holds
    TCPServer ..> TransactionManager : spawns per client
    TCPServer ..> ClientState : one per client
    TransactionManager ..> Dispatcher : replays queue on EXEC
    TransactionManager ..> Context : reads versions
    Context --> Database : points to
    Context --> PubSub : points to
    Context --> AuthConfig : points to
    Context ..> ClientState : current client
    Dispatcher ..> Context : passes to handlers

    Database *-- StringStore
    Database *-- ListStore
    Database *-- SortedSetStore
    Database *-- StreamStore

    StreamStore *-- StreamEntry
    StreamEntry *-- StreamID
    StreamStore ..> XAddResult : returns

    PubSub ..> ClientState : delivers to

    BasicCommands ..> Dispatcher : registers on
    ListCommands ..> Dispatcher : registers on
    SortedSetCommands ..> Dispatcher : registers on
    StreamCommands ..> Dispatcher : registers on
    GeoCommands ..> Dispatcher : registers on
    PubSubCommands ..> Dispatcher : registers on

    BasicCommands ..> StringStore : operates on
    ListCommands ..> ListStore : operates on
    SortedSetCommands ..> SortedSetStore : operates on
    StreamCommands ..> StreamStore : operates on
    GeoCommands ..> SortedSetStore : geohash in zset
    PubSubCommands ..> PubSub : operates on
    AuthCommands ..> Dispatcher : registers on
    AuthCommands ..> AuthConfig : checks password
    AuthCommands ..> ClientState : sets authenticated
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

---

## 4. Sequence: `PUBLISH` fan-out across connections

The publisher's thread writes into the subscriber's socket. The registry maps a
channel to the subscribers listening on it; delivery locks each subscriber's
`writeMutex` so it can't collide with that client's own replies.

```mermaid
sequenceDiagram
    participant A as Client A (subscriber)
    participant HA as handle_client A
    participant PS as PubSub
    participant HB as handle_client B
    participant B as Client B (publisher)

    A->>HA: SUBSCRIBE news
    HA->>PS: subscribe(A, "news")
    Note over PS: channelSubs["news"] += A_state
    PS-->>HA: (registered)
    HA-->>A: [subscribe, news, 1]
    Note over HA: parks in read()

    B->>HB: PUBLISH news "hi"
    HB->>PS: publish("news", "hi")
    Note over PS: look up channelSubs["news"]<br/>lock A.writeMutex, write frame
    PS-->>A: [message, news, hi]
    PS-->>HB: delivered = 1
    HB-->>B: (integer) 1
```
