#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "command/CommandDispatcher.hpp"
#include "protocol/RESPMessage.hpp"

class TransactionManager {
public:
    bool shouldHandle(const std::string& cmd) const;

    std::string handle(Context& context,
                       Dispatcher& dispatcher,
                       const std::string& cmd,
                       std::vector<RESPMessage> args);

private:
    struct QueuedCommand {
        std::string cmd;
        std::vector<RESPMessage> args;
    };

    bool active = false;
    std::vector<QueuedCommand> queue;

    // Keys watched by this connection -> their version at WATCH time.
    std::unordered_map<std::string, uint64_t> watched;
};