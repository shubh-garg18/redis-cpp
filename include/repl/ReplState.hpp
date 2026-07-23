#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>

struct ClientState;

// Master-side replica registry, plus this server's own role/link info. One
// shared instance owned by main, reached from handlers via Context::repl.
// Mirrors PubSub: a mutex-guarded set of connections. propagate() streams live
// writes to them; syncReplica() sends the one-time initial snapshot.
class ReplState {
public:
    enum class Role { MASTER, REPLICA };
    Role role = Role::MASTER;
    std::string replid;          // 40 hex chars, set once at startup
    std::string masterHost;      // set only if this server is a replica
    int masterPort = 0;

    // Register a new replica after seeding it with its initial snapshot. The
    // snapshot is BUILT (via buildSnapshot) and the replica REGISTERED both under
    // this lock, so no write's propagate() can slip in between the snapshot point
    // and registration — every write is either in the snapshot or streamed live,
    // never lost.
    void syncReplica(ClientState* c, const std::function<std::string()>& buildSnapshot);
    void dropReplica(ClientState* c);          // from Teardown on disconnect
    void propagate(const std::string& frame);  // write one frame to every replica
    int replicaCount();

private:
    std::mutex mutex;
    std::unordered_set<ClientState*> replicas;
};
