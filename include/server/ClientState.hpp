#pragma once

#include <mutex>
#include <string>
#include <unordered_set>

// Per-connection state, reachable from command handlers via Context::client.
// Owns the socket fd and a write mutex: other threads push bytes to this connection, so every write must go through
// writeMutex to avoid interleaving with this client's own replies.
struct ClientState {
    int fd;
    std::mutex writeMutex;

    std::unordered_set<std::string> channels;

    explicit ClientState(int f) : fd(f) {}

    bool inSubscribeMode() const {
        return !channels.empty();
    }
    size_t subscriptionCount() const {
        return channels.size();
    }
};
