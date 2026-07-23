# Concurrency — quick revision

Thread model: **one thread per connection** (`TCPServer` accepts, spawns a
`std::thread` per client). Shared state guarded by fine-grained locks, never one
global lock.

## Every lock, and what it guards

| Lock | Lives in | Guards |
| --- | --- | --- |
| `mutex` | each of StringStore / ListStore / SortedSetStore / StreamStore | that store's map |
| `versionMutex` | Database | `keyVersions` (WATCH counters) |
| `lruMutex` | Database | LRU list + `lruPos` map |
| `mutex` | PubSub | `channelSubs` registry |
| `mutex` | ReplState | `replicas` set |
| `mutex` | AofWriter | the `FILE*` |
| `writeMutex` | each ClientState | one connection's socket fd |

Blocking waits: `condition_variable` in **ListStore** (`BLPOP`) and **StreamStore**
(`XREAD BLOCK`). Waiter re-checks its predicate on each wake (`cv.wait(lock, pred)`),
so spurious wakeups are harmless; writers `notify_all` after mutating.

## The one rule that prevents deadlock

**Locks are only ever nested in one direction; the reverse never happens.**

- `recordAccess`: holds **lruMutex → store** (calls `typeOf`). Eviction collects
  victims under lruMutex, **releases it**, then `del`+`touch` (store/version) outside.
- `PubSub::publish`: holds **registry → writeMutex** (writes each subscriber).
- `ReplState::propagate`: holds **repl → writeMutex**.
- `ReplState::syncReplica`: holds **repl → store** (builds snapshot) **→ writeMutex**.

Why no cycle: a command handler takes its **store lock, releases it, then returns**;
only _after_ that does `executeCommand` call `propagate` (repl lock). So no thread
ever holds a store/write lock while reaching for a registry lock. `writeMutex` is
always a **leaf**. `versionMutex` is always a **leaf**.

## `writeMutex` — why it exists

One socket can be written by **two threads**: the connection's own replies _and_
a `PUBLISH`/`propagate` from another thread. Every write goes through that fd's
`writeMutex` so bytes never interleave.

## Races that are deliberately CLOSED

- **LRU phantom node** — `recordAccess` re-checks `typeOf` _under_ lruMutex before
  inserting, so a key `DEL`'d between the caller's read and the lock can't leave a
  stale front node.
- **Replication lost-write** — `syncReplica` builds the snapshot **and** registers
  the replica under one lock, so a write racing a `PSYNC` is either in the snapshot
  or streamed live, never dropped.
- **Freed `ClientState`** — the RAII `Teardown` calls `dropClient` + `dropReplica`
  (each under its registry lock) _before_ the `ClientState` is destroyed, so a
  concurrent `publish`/`propagate` can't write to freed memory.
- **WATCH** — `touch(key)` bumps a per-key version under versionMutex; `EXEC`
  compares. Eviction and every mutating handler `touch`, so a changed watched key
  aborts the transaction.

## Tradeoffs left OPEN (know these — they're interview gold)

- `publish`/`propagate` hold the **registry lock across the socket write**, and
  every write calls `replicaCount()` under that same lock → **one stalled peer
  stalls the subsystem**. Chosen to keep the freed-pointer race closed.
- **`EXEC` is not execution-time atomic** — queued commands run through the
  dispatcher with no lock held across them, so another client can interleave.
- **AOF append happens after the mutation, under a separate lock** → two concurrent
  same-key writes can be journaled in the opposite order.
- **PSYNC double-apply window** — a non-idempotent write (`RPUSH`/`INCR`) in flight
  exactly during a `PSYNC` can land in both snapshot and live stream (no offsets).
- **RESP parser** has no recursion/length cap → a crafted deeply-nested payload can
  crash the server.
