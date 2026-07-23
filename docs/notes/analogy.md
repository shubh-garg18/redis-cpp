# Analogies — quick revision

One image per piece, then how they connect.

## One analogy each

| Piece | Analogy |
| --- | --- |
| **TCPServer + thread-per-connection** | A restaurant host who seats each guest with their **own dedicated waiter** for the whole visit. |
| **RESP protocol** | A strict standardized **order-ticket format** everyone agrees on. |
| **RESPParser** | The waiter **reading the ticket** — and coping when half of it arrives, or three arrive stuck together. |
| **Database (facade)** | The **front desk** that routes each request to the right department without the guest knowing which. |
| **Typed stores** | Specialized **filing cabinets** — one for strings, one for lists, one for sorted sets, one for streams. |
| **One mutex per store** | Each cabinet has **its own lock**, so two clerks at different cabinets never wait on each other. |
| **condition_variable (BLPOP/XREAD)** | A **waiting room with a bell**: sleep on the bench until someone rings that a value arrived. |
| **TransactionManager (MULTI/EXEC)** | A **shopping basket** — you collect items, then check out all at once (or dump the basket). |
| **WATCH (optimistic lock)** | Noting the **version on a price tag**; at checkout you only commit if the tag hasn't changed. |
| **PubSub** | A **radio station**: whatever you broadcast reaches everyone tuned to that channel, no one else. |
| **ClientState.writeMutex** | Each guest's **single mail slot** — only one hand can post through it at a time. |
| **Auth (NOAUTH gate)** | A **bouncer** at the door: no password, no entry (except to say the password). |
| **AOF persistence** | A **diary** of every action, re-read after amnesia (restart) to redo everything. |
| **LRU eviction** | A **desk with room for N papers**: touch one → put it on top; out of room → toss the bottom one. |
| **Replication** | A **teacher and students**: copy what's already on the whiteboard once, then copy every new stroke. |
| **ReplState / ReplicaLink** | The teacher's **class roster** / a student **walking in and copying**. |
| **Context** | The **clipboard** each waiter carries — pointers to every department, plus "which table is mine." |

## How they connect (one guest's order, start to finish)

1. A **guest** (client) walks in and hands over an **order ticket** in the shared
   format (RESP). The **host** (`TCPServer`) seats them with a dedicated **waiter**
   (a thread running `handle_client`), who gets a **clipboard** (`Context`) noting
   this table.
2. The waiter **reads the ticket** (`RESPParser`), draining every ticket already on
   the tray before going back for more (pipelining).
3. **Bouncer** first (`auth` gate) → then the **basket** (`TransactionManager`): if
   the guest opened a basket (`MULTI`), items are collected, not run.
4. Otherwise the order goes to the **front desk** (`Database`), which files it in the
   right **cabinet** (a typed store), each with its own lock.
5. On any change the waiter also: writes a line in the **diary** (`AOF`), tells the
   **students** (`ReplState::propagate` to replicas), and — if it's a channel
   message — **broadcasts on the radio** (`PubSub`). All three share one rewrite
   (`canonicalWrite`).
6. Meanwhile the front desk keeps the **desk tidy** (`LRU`): every touched paper goes
   to the top; over the limit, the bottom is tossed.
7. A **new student** (replica) arriving copies the **whole whiteboard** once (the
   command snapshot) and then every new stroke (live propagation) — refusing to
   write anything themselves (READONLY).

The through-line: **the waiter (command handler) is the only one who talks to every
department; the departments never talk upward.** That's the "dependencies flow
downward" rule made physical.
