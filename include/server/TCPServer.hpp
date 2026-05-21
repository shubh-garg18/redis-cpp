#pragma once

#include "command/CommandDispatcher.hpp"

class TCPServer{
public:
    TCPServer(int port, Context* context, Dispatcher* dispatcher);
    void start();

private:
    int port;
    Context* context;
    Dispatcher* dispatcher;
};