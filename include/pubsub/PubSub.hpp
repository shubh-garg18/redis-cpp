#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ClientState;

// Cross-connection channel registry. PUBLISH runs on one client's thread but
// writes into other clients' sockets, so every method is guarded by one mutex
// and delivery holds it while writing (which also stops a subscriber from being
// dropped mid-publish).
class PubSub {
public:
    void subscribe(ClientState* c, const std::string& channel);
    void unsubscribe(ClientState* c, const std::string& channel);

    // Remove a connection from every channel (called on disconnect).
    void dropClient(ClientState* c);

    // Returns the number of clients that received the message.
    int publish(const std::string& channel, const std::string& message);

    // PUBSUB introspection.
    std::vector<std::string> listChannels();
    int numSubscribers(const std::string& channel);

private:
    std::mutex mutex;
    std::unordered_map<std::string, std::unordered_set<ClientState*>> channelSubs;
};
