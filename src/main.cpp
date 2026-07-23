#include "storage/Database.hpp"
#include "command/CommandDispatcher.hpp"
#include "command/BasicCommands.hpp"
#include "command/ListCommands.hpp"
#include "command/SortedSetCommands.hpp"
#include "command/StreamCommands.hpp"
#include "command/GeoCommands.hpp"
#include "command/PubSubCommands.hpp"
#include "command/AuthCommands.hpp"
#include "command/ReplCommands.hpp"
#include "pubsub/PubSub.hpp"
#include "auth/AuthConfig.hpp"
#include "persistence/AofWriter.hpp"
#include "repl/ReplState.hpp"
#include "protocol/RESPParser.hpp"
#include "server/TCPServer.hpp"
#include "server/ReplicaLink.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

// 40 random hex chars, like a real Redis run id (identifies this master).
static std::string randomReplId(){
    static const char* hex="0123456789abcdef";
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::string id;
    for(int i=0; i<40; i++) id+=hex[rng()%16];
    return id;
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
    std::string maxkeys = parseStringFlag(argc, argv, "--maxkeys", "0");

    // --replicaof <host> <port> takes two tokens, which parseStringFlag can't do.
    std::string masterHost;
    int masterPort = 0;
    bool isReplica = false;
    for(int i=1; i+2<argc; i++){
        if(std::strcmp(argv[i], "--replicaof") == 0){
            masterHost = argv[i+1];
            masterPort = std::atoi(argv[i+2]);
            isReplica = true;
            break;
        }
    }

    Database db;
    try { 
        db.setMaxKeys((size_t)std::stoul(maxkeys)); 
    } 
    catch(...) {}
    PubSub pubsub;
    AuthConfig auth;
    auth.requirepass=requirepass;
    AofWriter aof;
    ReplState repl;
    repl.replid=randomReplId();
    if(isReplica){
        repl.role=ReplState::Role::REPLICA;
        repl.masterHost=masterHost;
        repl.masterPort=masterPort;
    }
    Context ctx{&db, dir, dbfilename};
    ctx.pubsub=&pubsub;
    ctx.auth=&auth;
    ctx.repl=&repl;

    Dispatcher dispatcher;
    registerBasicCommands(dispatcher);
    registerListCommands(dispatcher);
    registerSortedSetCommands(dispatcher);
    registerStreamCommands(dispatcher);
    registerGeoCommands(dispatcher);
    registerPubSubCommands(dispatcher);
    registerAuthCommands(dispatcher);
    registerReplCommands(dispatcher);

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

    // As a replica, dial the master on a background thread; it syncs then streams.
    if(isReplica){
        std::thread(connectToMaster, masterHost, masterPort, port,
                    std::ref(dispatcher), std::ref(ctx)).detach();
    }

    try {
        TCPServer server(port, &ctx, &dispatcher);
        server.start();
    } catch(const std::exception& e){
        std::cerr<<"fatal: "<<e.what()<<"\n";
        // exit() rather than return: a detached replica-link thread may still
        // hold references to main's locals, which returning would destroy.
        std::exit(1);
    }
    return 0;
}
