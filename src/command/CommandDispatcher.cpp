#include "command/CommandDispatcher.hpp"
#include "protocol/RESPParser.hpp"
#include "persistence/AofWriter.hpp"
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

// Journal a successful write to the AOF. Non-deterministic commands are
// rewritten to what actually happened so replay reproduces the same state.
static void propagateToAof(Context& ctx, const std::string& cmd,
                           const std::vector<RESPMessage>& args,
                           const std::string& reply){
    if(!ctx.aof || !ctx.aof->isOpen()) return;   // also false during startup replay
    std::string upper=toUpper(cmd);
    if(!isWriteCommand(upper)) return;
    if(!reply.empty() && reply[0]=='-') return;   // command errored, nothing changed

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
        if(reply.rfind("*-1",0)==0) return;       // timed out, nothing popped
        std::string key;
        if(!firstBulk(reply, key)) return;
        argv={"LPOP", key};                        // replay as a plain pop of that key
    } else if(upper=="XADD"){
        std::string id;
        if(firstBulk(reply, id) && argv.size()>=3) argv[2]=id;  // real id, not '*'
    }

    std::string encoded=encodeRESPArrayHeader(argv.size());
    for(const auto& a: argv) encoded+=encodeRESPBulk(a);
    ctx.aof->append(encoded);
}

std::string Dispatcher::executeCommand(Context& context,
                                       const std::string& cmd,
                                       const std::vector<RESPMessage>& args
){
    auto it=handlers.find(toUpper(cmd));
    if(it==handlers.end()) return encodeRESPError("ERR unknown command '" + cmd + "'");
    std::string reply=it->second(context, args);
    propagateToAof(context, cmd, args, reply);
    return reply;
}