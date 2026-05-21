#include "command/BasicCommands.hpp"
#include "protocol/RESPParser.hpp"
#include "storage/StringStore.hpp"

static std::string handlePing(Context&, const std::vector<RESPMessage>& args){
    if(args.empty()) return encodeRESPSimple("PONG");
    return encodeRESPBulk(args[0].str);
}

static std::string handleEcho(Context&, const std::vector<RESPMessage>& args){
    if(args.size()!=1) return encodeRESPError("ERR wrong number of arguments for 'echo'");
    return encodeRESPBulk(args[0].str);
}

static std::string handleSet(Context& context, const std::vector<RESPMessage>& args){
    if(args.size()<2) return encodeRESPError("ERR wrong number of arguments for 'set'");

    const std::string& key=args[0].str;
    const std::string& value=args[1].str;
    int64_t expiry=0;

    // only PX is recognised; anything else is ignored.
    if(args.size()>3 && toUpper(args[2].str)=="PX"){
        try {
            int64_t ms=parse_int(args[3].str, 0, (int)args[3].str.size());
            expiry=StringStore::Date_now()+ms;
        } catch(...) {}
    }

    context.db->stringStore.set(key, value, expiry);
    return encodeRESPSimple("OK");
}

static std::string handleGet(Context& context, const std::vector<RESPMessage>& args){
    if(args.size()!=1) return encodeRESPError("ERR wrong number of arguments for 'get'");
    auto val=context.db->stringStore.get(args[0].str);
    if(!val) return encodeRESPNull();
    return encodeRESPBulk(*val);
}

static std::string handleDel(Context& context, const std::vector<RESPMessage>& args){
    if(args.empty()) return encodeRESPError("ERR wrong number of arguments for 'del'");
    std::vector<std::string> keys;
    for(const auto& m: args) keys.push_back(m.str);
    int count=context.db->stringStore.del(keys);
    return encodeRESPInteger(count);
}

void registerBasicCommands(Dispatcher& dispatcher){
    dispatcher.add("PING", handlePing);
    dispatcher.add("ECHO", handleEcho);
    dispatcher.add("SET",  handleSet);
    dispatcher.add("GET",  handleGet);
    dispatcher.add("DEL",  handleDel);
}
