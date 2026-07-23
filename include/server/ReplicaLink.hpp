#pragma once

#include <string>

#include "command/CommandDispatcher.hpp"

// Runs on a detached thread on a replica: connect to the master, do the
// PING/REPLCONF/PSYNC handshake, then apply the master's command stream (the
// snapshot, then live writes) through the dispatcher until the link drops.
void connectToMaster(const std::string& host, int port, int ownPort,
                     Dispatcher& dispatcher, Context& ctx);
