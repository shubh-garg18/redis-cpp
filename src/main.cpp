#include "storage/Database.hpp"
#include "command/CommandDispatcher.hpp"
#include "command/BasicCommands.hpp"
#include "command/ListCommands.hpp"
#include "server/TCPServer.hpp"

#include <iostream>
#include <stdexcept>

int main(){
    Database db;
    Context ctx{&db};

    Dispatcher dispatcher;
    registerBasicCommands(dispatcher);
    registerListCommands(dispatcher);

    try {
        TCPServer server(6379, &ctx, &dispatcher);
        server.start();
    } catch(const std::exception& e){
        std::cerr<<"fatal: "<<e.what()<<"\n";
        return 1;
    }
    return 0;
}
