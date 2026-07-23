#include "command/CommandDispatcher.hpp"
#include "protocol/RESPParser.hpp"
#include "persistence/AofWriter.hpp"
#include "repl/ReplState.hpp"
#include "storage/StringStore.hpp"

#include<string>
#include<cctype>
#include<vector>
#include<unordered_set>

std::string toUpper(const std::string& s){
    std::string cmd=s;
    for(char &c: cmd) c=(char)std::toupper((unsigned char)c);
    return cmd;
}

void Dispatcher::add(const std::string& cmd, CommandHandler fn){
    handlers[toUpper(cmd)]=fn;
}

// Commands that mutate the keyspace. This set gates AOF journaling, replica
// propagation, and the replica READONLY check — a new write command must be
// added here or it will silently skip all three.
static bool isWriteCommand(const std::string& upper){
    static const std::unordered_set<std::string> writes={
        "SET","DEL","INCR","LPUSH","RPUSH","LPOP","BLPOP",
        "ZADD","ZREM","XADD","GEOADD"};
    return writes.count(upper)>0;
}

// Pull the first bulk-string payload out of a reply: the id from XADD's bulk
// reply, or the popped key from BLPOP's array reply (which begins with it).
static bool firstBulk(const std::string& reply, std::string& out){
    size_t dollar=reply.find('$');
    if(dollar==std::string::npos) return false;
    size_t nl=reply.find("\r\n", dollar);
    if(nl==std::string::npos) return false;
    try {
        int len=std::stoi(reply.substr(dollar+1, nl-dollar-1));
        if(len<0) return false;
        out=reply.substr(nl+2, (size_t)len);
    } catch(...) { return false; }
    return true;
}

// Build the canonical RESP frame to journal/propagate for a successful write,
// or "" if this isn't a journalable write. Non-deterministic commands are
// rewritten to what actually happened, so both AOF replay and a replica
// reproduce the same state instead of re-rolling the dice.
static std::string canonicalWrite(const std::string& cmd,
                                  const std::vector<RESPMessage>& args,
                                  const std::string& reply){
    std::string upper=toUpper(cmd);
    if(!isWriteCommand(upper)) return "";
    if(!reply.empty() && reply[0]=='-') return "";   // command errored, nothing changed

    std::vector<std::string> argv;
    argv.push_back(upper);
    for(const auto& a: args) argv.push_back(a.str);

    if(upper=="SET"){
        // Relative PX is re-based to the restart clock on replay, so rewrite it
        // to an absolute PXAT deadline (which handleSet also accepts).
        for(size_t i=3; i+1<argv.size(); i++){
            if(toUpper(argv[i])=="PX"){
                try {
                    long long ms=std::stoll(argv[i+1]);
                    argv[i]="PXAT";
                    argv[i+1]=std::to_string(StringStore::Date_now()+ms);
                } catch(...) {}
                break;
            }
        }
    } else if(upper=="BLPOP"){
        if(reply.rfind("*-1",0)==0) return "";     // timed out, nothing popped
        std::string key;
        if(!firstBulk(reply, key)) return "";
        argv={"LPOP", key};                        // replay as a plain pop of that key
    } else if(upper=="XADD"){
        std::string id;
        if(firstBulk(reply, id) && argv.size()>=3) argv[2]=id;  // real id, not '*'
    }

    std::string encoded=encodeRESPArrayHeader(argv.size());
    for(const auto& a: argv) encoded+=encodeRESPBulk(a);
    return encoded;
}

std::string Dispatcher::executeCommand(Context& context,
                                       const std::string& cmd,
                                       const std::vector<RESPMessage>& args
){
    std::string upper=toUpper(cmd);

    // A replica is read-only to normal clients (client != null). Writes applied
    // by the master link come through with client == null and pass through.
    if(context.repl && context.repl->role==ReplState::Role::REPLICA
       && context.client!=nullptr && isWriteCommand(upper)){
        return encodeRESPError("READONLY You can't write against a read only replica.");
    }

    auto it=handlers.find(upper);
    if(it==handlers.end()) return encodeRESPError("ERR unknown command '" + cmd + "'");
    std::string reply=it->second(context, args);

    // One rewrite, two sinks: journal to the AOF and stream to any replicas.
    bool wantFrame=(context.aof && context.aof->isOpen())
                 || (context.repl && context.repl->replicaCount()>0);
    if(wantFrame){
        std::string frame=canonicalWrite(cmd, args, reply);
        if(!frame.empty()){
            if(context.aof && context.aof->isOpen()) context.aof->append(frame);
            if(context.repl && context.repl->replicaCount()>0) context.repl->propagate(frame);
        }
    }
    return reply;
}