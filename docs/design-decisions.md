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
predicate re-checks the data each wakeup, so spurious wakeup are harmless.

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

## 11. Strict compiler warnings

The whole project compiles under
`-Wall -Wextra -Wshadow -Wconversion -Wpedantic`, and the build is kept clean.

**Why.** The strict warning set catches narrowing conversions and shadowing
early, which matters most in the byte-twiddling protocol and ID-parsing code
where an implicit `int64_t`→`int` truncation would be a silent bug. Data is
in-memory by default; durability is opt-in through the AOF (see §14).

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

---

## 15. Tests drive a real server, not the code in-process

The suite in `tests/run_tests.sh` is an integration harness: it starts the
actual `redis_server` binary on a scratch port + temp dir, talks to it with
`redis-cli`, and asserts on the replies. There are no in-process unit tests.

**Why.** The whole product _is_ the wire behaviour — RESP framing, the command
dispatch, the connection lifecycle — so testing it the way a client sees it
covers the parts that matter with none of the mocking a unit test would need.
One script, no test framework to pull in, and it doubles as living
documentation of every command's expected reply. A non-zero exit on any failed
assertion makes it drop straight into CI later.

**Tradeoff.** A serial script driving one `redis-cli` at a time can't stage the
genuinely concurrent cases — cross-connection pub/sub delivery, or a `WATCH`
abort race between two clients — so those stay manual. And the benchmark
(`tests/run_bench.sh`) deliberately omits `LPUSH`/`LPOP` from its default set:
they expose the O(n) `std::vector` front-op cost from §10, which is worth
_demonstrating_ on demand but would otherwise dominate a routine run. The
benchmark's flat throughput across build types is the §1 thread-per-client
tradeoff made visible, not a thing to tune away.

---

## 16. Pipelining throughput: `TCP_NODELAY` and batched writes

Two changes, discovered by benchmarking rather than a priori, are what make
pipelining fast. Serial throughput (`-P 1`) was ~16k ops/sec; turning on
pipelining (`redis-benchmark -P 8`) at first made it _worse_ — ~4k — the
opposite of what pipelining should do. Fixing that took both of these together,
and pipelined throughput climbed to ~225k ops/sec at depth 16, past 400k higher.

**`TCP_NODELAY` on every socket.** The ~4k regression was the classic Nagle +
delayed-ACK interaction: a pipeline makes the server emit several small replies
back-to-back, and Nagle holds each one waiting for the previous segment's ACK,
which the client delays. Setting `TCP_NODELAY` on each accepted socket flushes
replies immediately; pipelined throughput jumped to ~120k at depth 16 from a
one-line `setsockopt`. It's exactly what real Redis does, and for the same
reason. The trade is deliberate: Nagle exists to coalesce tiny packets, so
disabling it sends a burst of small replies as more, smaller TCP segments — the
right call for a latency-dominated request/response cache, but not a free win.

**One `write()` per drained batch.** The connection loop reads bytes, then
drains _every_ complete frame already buffered before going back to `read()`.
Once `TCP_NODELAY` was on, the next cost was syscalls: writing a reply per
command meant 16 `write()`s for a `-P 16` batch. Appending each frame's reply to
one `outbuf` and writing the whole batch once cut that to a single syscall and
roughly _doubled_ throughput again (~120k → ~225k at depth 16). Serial traffic
is unaffected — one command per read is still one write.

**Tradeoff.** Replies for a batch are held until the whole batch is drained, so
if a command mid-pipeline blocks (a `BLPOP` with no data), the replies queued
before it wait for it to unblock. Pipelining a blocking command is a pathological
mix, and the alternative — flushing before every possibly-blocking call — would
throw away the batching win, so the buffered-until-drain behaviour is kept.

---

## 17. LRU eviction: a hand-rolled list above the typed stores

With `--maxkeys N`, the keyspace is capped at `N` keys; once it's exceeded, the
least-recently-used key is evicted. The recency order is a **hand-written
doubly-linked list** (a `LruNode` struct with `prev`/`next` and two sentinels),
paired with a `key -> LruNode*` map for O(1) lookup. Every command that touches a
key calls `Database::recordAccess`, which splices that key to the front in O(1);
when the map outgrows the cap, nodes are dropped from the tail. It lives in
`Database`, above the typed stores, because a key can be any type and eviction
has to reach across all of them.

**Why a raw list, not `std::list`.** Partly to show the structure honestly — this
is the classic map + doubly-linked-list LRU — and partly because raw `LruNode*`
in the map stay valid across rehashes, which `std::list` iterators also do but
without making the pointer surgery visible. Access is O(1) whether it's a hit
(splice) or an insert, and eviction is O(1) from the tail.

**Concurrency.** Recency is guarded by its own mutex. `recordAccess` re-checks
the key still exists (`typeOf`) _under_ that lock before inserting a node —
otherwise a key deleted between the caller's read and here would get a fresh
node spliced to the _front_, a phantom that never ages out. Eviction _collects_
victim keys under the lock, releases it, and only then deletes them from the
stores. So the lock order is always recency → store (the `typeOf` probe nests in
that same direction, and deletion never holds a store lock while taking the
recency one), and there's no deadlock with the per-store mutexes. An evicted key
is `touch`ed so a `WATCH` on it aborts the transaction, exactly like a `DEL`.

**Tradeoffs.** True LRU (reads refresh recency, not just writes), so a hot
read-only key never ages out — at the cost of a `recordAccess` call, including a
`typeOf` probe, on read paths too. The cap is a **key count**, not a byte
budget: simple and exact, but it doesn't bound memory when values differ wildly
in size (real Redis uses `maxmemory` with sampled-LRU approximation; this is
exact LRU with a count). Under this cap the order is exact per operation; the one
soft edge is that a key deleted concurrently with its _own_ eviction can briefly
re-appear as a stale node, harmlessly reclaimed on a later eviction (its delete
is then a no-op) — so LRU stays exact in the common case and is only approximate
under that specific race.

---

## 18. Replication: initial sync as a command snapshot, not RDB

A replica keeps a live copy of a master's dataset. It connects out, does a
`PING`/`REPLCONF`/`PSYNC` handshake, receives the master's current data, then
applies every subsequent write the master streams to it. `--replicaof <host>
<port>` makes a server a replica; `INFO replication` reports roles; a replica
rejects direct client writes with `-READONLY`.

**Why a command snapshot.** Real Redis's `PSYNC` ships the initial dataset as an
**RDB binary blob**. There is no RDB writer here, and building one purely to seed
replication is low-value binary-format busywork. So the initial sync is instead
a stream of **RESP write commands** — `Database::dumpAsCommands()` re-encodes the
whole keyspace as `SET`/`RPUSH`/`ZADD`/`XADD` (the same shape the AOF produces) —
which the replica simply runs through its normal dispatcher. Live writes then
stream on top through the _same_ `canonicalWrite` path the AOF already uses: one
rewrite, two sinks (journal + replicas).

**Design details worth knowing.**

- **Read-only via `client != nullptr`, not a flag.** Normal clients carry a
  `ClientState`; the master link and AOF replay apply writes with `client ==
  null`. The `READONLY` gate in `executeCommand` keys on that, and sits _before_
  `EXEC` replay so a `MULTI` on a replica is rejected too.
- **Snapshot built under the replica-registry lock.** `syncReplica` builds the
  snapshot _and_ registers the replica while holding `ReplState`'s mutex, so no
  write's `propagate()` can slip in between the snapshot point and registration —
  every write is either captured in the snapshot or streamed live, never lost.

**Tradeoffs — deliberate cuts for a learning build.**

- **No interop with real Redis, either direction.** `+FULLRESYNC` is not followed
  by an RDB payload, so only this project's own `ReplicaLink` can consume the
  stream (and it can't sync from a real master). Correct for the learning goal.
- **A slow replica can stall master writes.** `propagate` (and `syncReplica`)
  write to sockets while holding the registry mutex — and every write command
  checks `replicaCount()` under that same mutex — so one stuck replica back-
  pressures all writes, and a large snapshot blocks propagation for its whole
  transmission. Same tradeoff as pub/sub (§12), accepted for the same reason;
  the alternative (writing outside the lock) reopens the freed-`ClientState`
  race the lock closes.
- **Residual double-apply window.** Closing the lost-write race leaves a tiny
  window where a non-idempotent write (`RPUSH`, `INCR`) in flight exactly during
  a `PSYNC` can land in both the snapshot and the live stream. Real Redis dedupes
  this with replication offsets; those are omitted here (`master_repl_offset` is
  always 0, `REPLCONF ACK` is swallowed).
- **Omitted:** partial resync / backlog (`+CONTINUE`), `WAIT`, offset accounting,
  and reconnect — on a link drop the replica goes stale rather than re-dialing.
  The master address is parsed as a dotted-quad IPv4 (no hostname resolution).
