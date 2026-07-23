#include "command/ReplCommands.hpp"

#include <string>
#include <vector>

#include "protocol/RESPParser.hpp"
#include "repl/ReplState.hpp"

// REPLCONF: the replica announces its port/capabilities during the handshake
// (master just acks), and later sends REPLCONF ACK <offset> while streaming,
// which carries no reply — swallow it so the master doesn't answer mid-stream.
static std::string handleReplconf(Context&, const std::vector<RESPMessage>& args){
    if(!args.empty() && toUpper(args[0].str)=="ACK") return "";
    return encodeRESPSimple("OK");
}

// PSYNC: the whole initial sync. Reply the FULLRESYNC line + a command snapshot
// of the current dataset, then register this connection so live writes reach it.
// syncReplica writes the payload and registers atomically, so nothing interleaves
// ahead of the snapshot; we return "" because the payload is already on the wire.
static std::string handlePsync(Context& context, const std::vector<RESPMessage>&){
    if(!context.repl || !context.client)
        return encodeRESPError("ERR replication not available");

    // The snapshot is built inside syncReplica (under the replica lock) so a
    // write racing this PSYNC can't be dropped between snapshot and registration.
    context.repl->syncReplica(context.client, [&](){
        std::string payload="+FULLRESYNC "+context.repl->replid+" 0\r\n";
        for(const auto& cmd : context.db->dumpAsCommands()) payload+=encodeRESPArray(cmd);
        return payload;
    });
    return "";
}

// INFO replication: role and replica/master info. Only the replication section
// is implemented; the handshake and tests read role/connected_slaves/replid.
static std::string handleInfo(Context& context, const std::vector<RESPMessage>&){
    bool isReplica=context.repl && context.repl->role==ReplState::Role::REPLICA;

    std::string body="# Replication\r\n";
    body+="role:"+std::string(isReplica ? "slave" : "master")+"\r\n";
    if(context.repl){
        if(isReplica){
            body+="master_host:"+context.repl->masterHost+"\r\n";
            body+="master_port:"+std::to_string(context.repl->masterPort)+"\r\n";
        }
        body+="connected_slaves:"+std::to_string(context.repl->replicaCount())+"\r\n";
        body+="master_replid:"+context.repl->replid+"\r\n";
        body+="master_repl_offset:0\r\n";
    }
    return encodeRESPBulk(body);
}

void registerReplCommands(Dispatcher& dispatcher){
    dispatcher.add("REPLCONF", handleReplconf);
    dispatcher.add("PSYNC", handlePsync);
    dispatcher.add("INFO", handleInfo);
}
