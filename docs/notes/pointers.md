# Pointers & references — quick revision

Where every pointer/reference/indirection is, and the one-line reason.

## `Context` service pointers

`Context` holds **raw pointers** to shared, long-lived services:
`Database*`, `ClientState*`, `PubSub*`, `AuthConfig*`, `AofWriter*`, `ReplState*`.

- **Raw, not smart** — they don't own anything; `main` owns the real objects on
  its stack for the whole process lifetime. No ownership → no `unique_ptr`.
- **Forward-declared** in `CommandDispatcher.hpp` (`struct ClientState; class
  PubSub; class ReplState; ...`) — the header only stores pointers, so it needs no
  full definition. This is what stops an upward `command → server` include
  dependency. The `.cpp` files that _dereference_ them include the real headers.
- **`ClientState* client`** is per-connection; the others are shared singletons.

## The per-connection `Context` COPY

`handle_client` does `Context ctx = *context; ctx.client = &client;`. A **copy by
value**, then stamp in this connection's `ClientState*`. So every thread shares the
same `Database`/`PubSub`/`ReplState` (pointers copied) but has its own `client`.

## `ClientState*` in the registries

- `PubSub`: `unordered_map<string, unordered_set<ClientState*>>` — a channel → its
  subscribers. The pointer is a connection's cross-thread **identity**.
- `ReplState`: `unordered_set<ClientState*> replicas` — same idea for replicas.
- Both are swept on disconnect by `Teardown` (`dropClient`/`dropReplica`) **before**
  the `ClientState` is destroyed → no dangling pointer.

## Hand-rolled LRU list

- `struct LruNode { string key; LruNode* prev; LruNode* next; }` — raw `prev`/`next`
  links, two **sentinel** nodes (head/tail) so there are no null edge cases.
- `unordered_map<string, LruNode*> lruPos` — key → its node, for O(1) find.
- Raw `LruNode*` deliberately: they stay valid across map rehashes, and the pointer
  surgery (`unlink`/`pushFront`) is the point of the exercise.

## Function-pointer-ish indirection

- **`CommandHandler` = `std::function<string(Context&, const vector<RESPMessage>&)>`**
  — the dispatcher is a `name → std::function` map. Handlers are `static` in their
  `.cpp`; only `register*Commands` is exposed.
- **`syncReplica(c, const std::function<string()>& buildSnapshot)`** — a callback so
  `repl` can build the snapshot **under its lock** without depending on `Database`
  (the lambda, defined in `command`, knows `Database`).

## References (`&`) vs pointers

- Handlers take **`Context&`** (never null, always present) — reference.
- Services that _might_ be absent are **pointers** with `nullptr` checks
  (`ctx.aof`, `ctx.repl`, `ctx.client`). The **READONLY gate keys on
  `ctx.client != nullptr`** to tell a real client apart from a master-link/AOF apply.
- `TCPServer` stores `Context*` / `Dispatcher*`; the accept loop passes them by
  pointer into each thread's `handle_client`.

## Raw sockets

- `int fd` everywhere (POSIX). `ClientState.fd` is the connection; `ReplicaLink`
  owns its outbound `fd` alone (no `writeMutex` — single owner).
