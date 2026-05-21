#pragma once

#include "command/CommandDispatcher.hpp"

void handle_client(int connection, Context* context, Dispatcher& dispatcher);