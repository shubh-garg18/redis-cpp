# Design Decisions

Not an exhaustive log — just the choices that shaped the codebase and the
tradeoffs each one accepts. Where a decision differs from real Redis, that's
called out.

---

## 1. Thread-per-connection, not an event loop

Every accepted connection gets its own `std::thread` running `handle_client`.
Real Redis is famously single-threaded around an `epoll` event loop.

**Why.** The threaded model is dramatically simpler to reason about: each
handler runs top-to-bottom on its own stack, and a blocking call like `BLPOP`
can just _block_ the thread instead of registering a continuation. The OS
scheduler does the multiplexing for free.

**Tradeoff.** Each thread costs ~1 MB of stack, so this scales to hundreds of
clients and falls over around the C10K mark. An event loop handles 100k+
connections in one thread — but then every handler must be non-blocking, which
would turn `BLPOP`/`XREAD` into explicit state machines. For a learning project,
readability wins.

---

## 2. Typed stores behind a `Database` facade

Rather than one `unordered_map<string, variant<...>>`, each data type lives in
its own store (`StringStore`, `ListStore`, `SortedSetStore`, `StreamStore`), and
`Database` is a thin facade over them.

**Why.** Each store picks the container that fits its type and owns its own
locking, with no giant tagged union to switch on in every command. Cross-type
operations — `DEL`, `TYPE`, `KEYS`, `hasWrongType` — are written _once_ in the
facade, so adding a data type is a local change plus a few facade edits, not a
scavenger hunt.

**Tradeoff.** A key's type is discovered by asking each store in turn, which is
a handful of hash lookups rather than one. Negligible here, and worth it for
keeping the type logic out of the command handlers.

---

## 3. One mutex per store, not one global lock

Each store guards its own map with its own `std::mutex`.

**Why.** Two clients hitting a list and a sorted set don't contend. Lock scope
stays narrow and local to the store that owns the data.

**Tradeoff.** There is no cross-key or cross-type atomicity below the command
level — a single command is atomic within its store, but the server cannot lock
"the whole keyspace" for a multi-key invariant. Transactions paper over this at
a higher level (see §6) using versioning rather than locking.

---

## 4. Handlers return RESP-encoded strings directly

A `CommandHandler` is `std::string(Context&, const vector<RESPMessage>&)`. It
does its own encoding and hands back bytes ready for the socket.

**Why.** No intermediate "reply object" layer to define, build, and then
serialize. What you see in a handler is exactly what goes on the wire, which
makes the encoding easy to follow.

**Tradeoff.** Handlers are coupled to the wire format, and nested replies (a
stream entry is `[id, [field, value, ...]]`) are assembled by hand with
`encodeRESPArrayHeader` + concatenation instead of a structured builder. Fine at
this scale; a builder would earn its keep only with many more compound replies.

---

## 5. Blocking commands block in the handler thread

`BLPOP` and blocking `XREAD` wait on the owning store's `condition_variable`;
the writer side (`RPUSH`, `XADD`) calls `notify_all` after releasing the lock.

**Why.** This falls straight out of decision §1 — with a thread per client, the
natural way to wait is to actually sleep the thread. The `condition_variable`
predicate re-checks the data each wakeup, so spurious wakeups are harmless.

**Tradeoff.** A blocked client holds a whole thread hostage for the duration.
Under the event-loop model this would be a parked continuation costing almost
nothing. Consistent with the threading choice, so accepted.

---

## 6. Transactions use optimistic locking, not held locks

`WATCH` records each watched key's current version. `Database` keeps a
monotonic per-key counter that every mutating handler bumps via `touch(key)`. On
`EXEC`, if any watched key's version moved, the whole transaction aborts with a
nil array; otherwise the queued commands run in order.

**Why.** Holding locks across a client's think-time (between `WATCH` and `EXEC`)
would be a recipe for deadlock and head-of-line blocking. Versioning lets the
server detect interference after the fact without ever holding a cross-key lock
— the same compare-and-set idea real Redis uses.

**Tradeoff.** It's optimistic: a transaction can be rejected and retried under
contention rather than waiting its turn. That's the intended semantic, and it
keeps the storage layer lock-simple.

---

## 7. `TransactionManager` intercepts before the dispatcher

The transaction state machine is a **per-connection** object that sees every
command before the dispatcher does. `MULTI`/`EXEC`/`DISCARD`/`WATCH`/`UNWATCH`
and any queued command are handled there; everything else falls through to the
dispatcher.

**Why.** Command queueing is connection-local state, so it belongs at the
connection layer, not smeared across the shared command table. The dispatcher
stays a pure, stateless name→handler map.

**Tradeoff.** The connection loop has to consult two things (the manager, then
the dispatcher) instead of one. A small, well-contained cost for keeping the two
concerns separate.

---

## 8. Streams: append-only vector, cached top, sentinel IDs

A stream is a `vector<StreamEntry>` (append-only, so always sorted) plus a
cached `top` ID. Auto-generated ID parts are signalled to the store with `-1`
sentinels, and each entry's fields are a flat `[field, value, ...]` vector.

**Why.** Because IDs strictly increase, appending keeps the vector sorted for
free — range queries are ordered scans and no sort is ever needed. The cached
`top` makes validating a new ID and resolving the `$` cursor O(1), and it
survives an empty stream where "the last element" doesn't exist. Fields stay a
flat vector because RESP echoes them back in insertion order; a map would
destroy that order for no benefit, since they're never looked up by name.

**Tradeoff.** Sentinels (`ms = -1`, `seq = -1`) are less self-documenting than
`std::optional`, chosen to match the project's plain-types style. `XRANGE` is a
linear scan rather than a binary search — fine for the sizes this handles.

---

## 9. `WRONGTYPE` enforced through the `ValueType` enum

Every typed command checks `hasWrongType(key, ValueType::X)` up front, which the
facade answers from a single generic `typeOf` lookup.

**Why.** One place defines "what type is this key," and every command reuses it,
so type safety is consistent and a new type opts in just by extending the enum
and `typeOf`.

**Tradeoff.** This is _stricter_ than real Redis on one edge: real Redis lets
`SET` overwrite a key of any type, whereas here the `hasWrongType` gate is
applied uniformly. The stricter, more predictable behaviour was chosen
deliberately and matches across all types.

---

## 10. Lists are backed by `std::vector`, not `std::deque`

**Why.** `std::vector` is the plainest sequence container, and the code reads
straightforwardly with it.

**Tradeoff.** `LPUSH` and `LPOP` operate at the front, which is O(n) on a vector
because every element shifts. A `std::deque` would give O(1) at both ends. This
is a known, accepted simplification — worth revisiting if lists ever see heavy
head traffic.

---

## 11. In-memory only, strict warnings

There is no persistence yet — the dataset lives entirely in RAM and is lost on
restart (RDB/AOF are on the roadmap). The whole project compiles under
`-Wall -Wextra -Wshadow -Wconversion -Wpedantic`.

**Why.** Persistence is a large subsystem best added deliberately rather than
half-built. The strict warning set catches narrowing conversions and shadowing
early, which matters most in the byte-twiddling protocol and ID-parsing code.

---

## 12. Pub/Sub delivers across connections via a shared registry

Pub/Sub is the first feature where a handler must write to a connection _other_
than the caller's. It adds a per-connection `ClientState` (socket fd, a
`writeMutex`, and the subscribed channels), reachable from handlers through a
per-connection copy of `Context`, plus a shared `PubSub` registry mapping
`channel → {ClientState*}`. `PUBLISH` looks the channel up and writes the
message frame straight into each subscriber's socket. Pattern subscriptions
(`PSUBSCRIBE`) were intentionally left out to keep the feature on the delivery
mechanism rather than glob matching.

**Why.** No new keyspace type is involved, so it doesn't belong in `Database`;
it's a separate cross-connection service. Giving each connection a `ClientState`
that other threads can reach is the smallest thing that makes fan-out possible,
and it doubles as the scaffold auth/replication will reuse.

**Tradeoff.** Two threads can write to one socket — a `PUBLISH` from another
connection and that client's own replies — so every write goes through the
connection's `writeMutex`, with lock order always registry → `writeMutex` (never
the reverse, so no deadlock). Delivery holds the single registry mutex while
writing to sockets, which is simple and keeps a subscriber from being freed
mid-publish, but means a stalled subscriber can back-pressure the whole
subsystem. Real Redis avoids this with non-blocking sockets and per-client
output buffers; the blocking version is accepted here for the same reason as the
thread-per-client model.

---

## 13. Auth is a gate in the connection loop, not a per-command check

`--requirepass` sets one server-wide `AuthConfig`, and each connection carries a
`ClientState::authenticated` bool. The check lives once in `handle_client`,
**before** the transaction manager and dispatcher: if auth is enabled and the
connection isn't authenticated, every command except `AUTH` is rejected with
`NOAUTH`.

**Why.** Putting the gate first — ahead of `TransactionManager::shouldHandle` —
is what makes it airtight with almost no code. An unauthenticated client's
`MULTI`, `WATCH`, or `SUBSCRIBE` all hit the `NOAUTH` branch, so there's no way
to open a transaction and smuggle commands past the check, and no handler needs
its own auth logic. Auth reused the pub/sub scaffold wholesale: the flag is just
another field on `ClientState`, reached through the per-connection `Context`
copy, so the whole feature is one gate plus one command — no new library, no
locking.

**Tradeoff.** The password compare (`AuthCommands.cpp`) is a plain
`std::string` comparison, not constant-time, so it's a theoretical timing
oracle. Network jitter swamps the signal and real Redis only hardened this
recently, so it's left as-is with a comment rather than gold-plated. Only the
implicit `default` user is modelled, and the pre-auth allow-list is `AUTH`-only
(real Redis also permits `HELLO`/`RESET`/`QUIT`, none of which this clone
implements).

---

## 14. AOF: journal at the dispatcher, and canonicalize non-deterministic writes

With `--appendonly yes`, every write is appended to a file as a RESP array and
replayed at startup. The append hook lives at the end of
`Dispatcher::executeCommand`, and non-deterministic commands are rewritten to
their concrete effect before being written: `XADD *`→ the assigned id,
`BLPOP`→ `LPOP <popped-key>`, `SET … PX`→ `SET … PXAT <absolute>`.

**Why the dispatcher.** `EXEC` runs each queued command back through
`executeCommand`, so hooking there captures transaction writes for free — a hook
in the connection loop would miss them. Rewriting non-deterministic input is
what makes replay faithful: a journal that re-ran `XADD *` or a relative `PX`
would drift from the original state every restart. Replay runs with `ctx.aof`
still null so it can't re-append into the file it's reading; only afterward is
the writer opened.

**Tradeoffs.** Three, all accepted deliberately for a thread-per-client build:

- **Append is outside the mutation lock.** A handler mutates under its store's
  mutex, then `propagateToAof` appends under the AOF's own mutex — so two
  concurrent writes to the same key can be journaled in the reverse order they
  were applied. The only real fix is a global write lock, which would undo
  decision §3 (one mutex per store); not worth it here.
- **`fflush`, not `fsync`.** A write is pushed to the OS page cache each time,
  surviving a process crash but not an OS crash / power loss. Real Redis's
  `appendfsync everysec` is the middle ground; flush-only keeps it simple.
- **Monotonic expiry clock.** `PXAT` deadlines are correct across a process
  restart (what AOF protects against) but not across a machine reboot, since the
  monotonic clock resets. A wall-clock switch in `StringStore` would close this
  but changes live TTL semantics, so it's left for a future pass.
