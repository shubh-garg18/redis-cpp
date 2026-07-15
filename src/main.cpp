#include "storage/Database.hpp"
#include "command/CommandDispatcher.hpp"
#include "command/BasicCommands.hpp"
#include "command/ListCommands.hpp"
#include "command/SortedSetCommands.hpp"
#include "command/StreamCommands.hpp"
#include "command/GeoCommands.hpp"
#include "server/TCPServer.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <chrono>

static int parsePort(int argc, char** argv){
    int port = 6379;
    for(int i=1; i+1<argc; i++){
        if(std::strcmp(argv[i], "--port") == 0){
            int candidate = std::atoi(argv[i+1]);
            if(candidate > 0 && candidate <= 65535) port = candidate;
        }
    }
    return port;
}

static std::string parseStringFlag(int argc, char** argv, const char* flag, const char* fallback){
    for(int i=1; i+1<argc; i++){
        if(std::strcmp(argv[i], flag) == 0) return argv[i+1];
    }
    return fallback;
}

int main(int argc, char** argv){
    int port = parsePort(argc, argv);
    std::string dir = parseStringFlag(argc, argv, "--dir", "/tmp/redis-data");
    std::string dbfilename = parseStringFlag(argc, argv, "--dbfilename", "dump.rdb");

    Database db;
    Context ctx{&db, dir, dbfilename};

    Dispatcher dispatcher;
    registerBasicCommands(dispatcher);
    registerListCommands(dispatcher);
    registerSortedSetCommands(dispatcher);
    registerStreamCommands(dispatcher);
    registerGeoCommands(dispatcher);

    try {
        TCPServer server(port, &ctx, &dispatcher);
        server.start();
    } catch(const std::exception& e){
        std::cerr<<"fatal: "<<e.what()<<"\n";
        return 1;
    }
    return 0;
}
