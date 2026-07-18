#include "storage/Database.hpp"
#include "command/CommandDispatcher.hpp"
#include "command/BasicCommands.hpp"
#include "command/ListCommands.hpp"
#include "command/SortedSetCommands.hpp"
#include "command/StreamCommands.hpp"
#include "command/GeoCommands.hpp"
#include "command/PubSubCommands.hpp"
#include "command/AuthCommands.hpp"
#include "pubsub/PubSub.hpp"
#include "auth/AuthConfig.hpp"
#include "persistence/AofWriter.hpp"
#include "protocol/RESPParser.hpp"
#include "server/TCPServer.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
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

// Replay the append-only file through the dispatcher. ctx.aof is still null
// here, so the replayed writes are not fed back into the AOF.
static void replayAOF(const std::string& path, Dispatcher& dispatcher, Context& ctx){
    std::ifstream file(path, std::ios::binary);
    if(!file.is_open()) return;
    std::string buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    while(!buffer.empty()){
        ParseResult r;
        try {
            r=RESPParser(buffer);
        } catch(const std::exception&){
            break;  // corrupt tail — stop replaying
        }
        if(!r.ok) break;  // truncated tail
        if(r.value.type==RESPMessage::Type::ARR && !r.value.arr.empty()){
            const std::string& cmd=r.value.arr[0].str;
            std::vector<RESPMessage> args(r.value.arr.begin()+1, r.value.arr.end());
            dispatcher.executeCommand(ctx, cmd, args);
        }
        buffer.erase(0, (size_t)r.len);
    }
}

int main(int argc, char** argv){
    int port = parsePort(argc, argv);
    std::string dir = parseStringFlag(argc, argv, "--dir", "/tmp/redis-data");
    std::string dbfilename = parseStringFlag(argc, argv, "--dbfilename", "dump.rdb");
    std::string requirepass = parseStringFlag(argc, argv, "--requirepass", "");
    std::string appendonly = parseStringFlag(argc, argv, "--appendonly", "no");
    std::string appendfilename = parseStringFlag(argc, argv, "--appendfilename", "appendonly.aof");

    Database db;
    PubSub pubsub;
    AuthConfig auth;
    auth.requirepass=requirepass;
    AofWriter aof;
    Context ctx{&db, dir, dbfilename};
    ctx.pubsub=&pubsub;
    ctx.auth=&auth;

    Dispatcher dispatcher;
    registerBasicCommands(dispatcher);
    registerListCommands(dispatcher);
    registerSortedSetCommands(dispatcher);
    registerStreamCommands(dispatcher);
    registerGeoCommands(dispatcher);
    registerPubSubCommands(dispatcher);
    registerAuthCommands(dispatcher);

    // AOF is the source of truth when enabled: replay it (with ctx.aof still
    // null so nothing is re-appended), then keep it open for live appends.
    if(toUpper(appendonly)=="YES"){
        std::string aofPath=dir+"/"+appendfilename;
        replayAOF(aofPath, dispatcher, ctx);
        if(!aof.open(aofPath)){
            std::cerr<<"fatal: cannot open AOF "<<aofPath<<"\n";
            return 1;
        }
        ctx.aof=&aof;
    }

    try {
        TCPServer server(port, &ctx, &dispatcher);
        server.start();
    } catch(const std::exception& e){
        std::cerr<<"fatal: "<<e.what()<<"\n";
        return 1;
    }
    return 0;
}
