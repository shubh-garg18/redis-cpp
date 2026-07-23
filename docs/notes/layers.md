# Layers — quick revision

Seven libraries, dependencies flow **downward only**. `server → command →
{protocol, storage, pubsub, persistence, repl}`.

```text
server  →  command  →  protocol   (parse/encode RESP)
                    →  storage    (the keyspace)
                    →  pubsub      (channel registry)
                    →  persistence (AOF)
                    →  repl        (replica registry)
```

## Who lives where, and why

| Layer | Holds | Why here |
| --- | --- | --- |
| **protocol** | `RESPParser`, encoders, `parse_int` | lowest layer, depends on nothing; everyone encodes/decodes |
| **storage** | 4 typed stores + `Database` facade | owns all keyspace data + its locks; cross-type ops (`DEL`/`TYPE`/`KEYS`) written once in the facade |
| **pubsub** | `PubSub` registry | cross-connection messaging — **no keyspace**, so _not_ in Database |
| **persistence** | `AofWriter` | dumb file writer; the _journaling logic_ is NOT here |
| **repl** | `ReplState` (replica registry) | master-side fan-out; dep-free of `command` on purpose |
| **command** | `Dispatcher`, all `*Commands`, `Context` | maps name→handler; owns the write-path hooks |
| **server** | `TCPServer`, `ClientSession`, `TransactionManager`, `ReplicaLink` | sockets, per-connection loop, MULTI/EXEC state |

## The "why not somewhere else" decisions (the ones you get asked)

- **Pub/Sub is its own lib, not part of Database** — it holds channels, not keys.
- **AOF/replica journaling lives in `command` (`executeCommand`), not `persistence`
  or `ClientSession`** — because `EXEC` replays queued writes *through the
  dispatcher*; hooking there captures transaction writes for free.
- **`repl` depends only on the `ClientState` header, NOT on `command`** — so
  `command` can depend on `repl` without a cycle. The catch: the outbound replica
  connector (`ReplicaLink`) needs the `Dispatcher`, so it can't live in `repl` — it
  goes in **`server`** (which already links `command`).
- **READONLY gate + write fan-out sit in `executeCommand`, not `ClientSession`** —
  same reason as the AOF hook: they must see `EXEC`-replayed writes.
- **One mutex per store, not one global** — a list op and a zset op don't contend.
  Cost: no cross-key atomicity below the command level (transactions use versioning,
  not locking, to paper over this).

## `Context` — the glue

A per-connection **copy** of `Context` carries pointers to all shared services:
`Database* db`, `ClientState* client`, `PubSub* pubsub`, `AuthConfig* auth`,
`AofWriter* aof`, `ReplState* repl`. `handle_client` copies the shared `Context`
and stamps in _this_ connection's `ClientState*`, so a handler can reach both the
shared world and "who am I."

## Adding a feature — where it goes

- New data type → `storage/` (new store + `Database` facade edits) + `command/`
  (new `*Commands`) + one `register*` call in `main`.
- New cross-connection service (like pub/sub, repl) → its own tiny lib +
  a `Context` pointer, reached through the per-connection copy.
