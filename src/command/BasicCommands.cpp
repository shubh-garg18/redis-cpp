#include "command/BasicCommands.hpp"
#include "protocol/RESPParser.hpp"
#include "storage/StringStore.hpp"

static std::string wrongTypeError() {
    return encodeRESPError("WRONGTYPE Operation against a key holding the wrong kind of value");
}

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

    context.db->clearKey(key);
    context.db->stringStore.set(key, value, expiry);
    context.db->touch(key);
    return encodeRESPSimple("OK");
}

static std::string handleGet(Context& context, const std::vector<RESPMessage>& args){
    if(args.size()!=1) return encodeRESPError("ERR wrong number of arguments for 'get'");

    if (context.db->hasWrongType(args[0].str, ValueType::String)) return wrongTypeError();

    auto val=context.db->stringStore.get(args[0].str);
    if(!val) return encodeRESPNull();
    return encodeRESPBulk(*val);
}

static std::string handleDel(Context& context, const std::vector<RESPMessage>& args){
    if(args.empty()) return encodeRESPError("ERR wrong number of arguments for 'del'");
    int count=0;
    for(const auto& m: args){
        if(context.db->del({m.str}) > 0){
            context.db->touch(m.str);
            count++;
        }
    }
    return encodeRESPInteger(count);
}

static std::string handleIncr(Context& context, const std::vector<RESPMessage>& args) {
    if (args.size() != 1) return encodeRESPError("ERR wrong number of arguments for 'incr'");

    if (context.db->hasWrongType(args[0].str, ValueType::String)) return wrongTypeError();

    try {
        int64_t val = context.db->stringStore.incr(args[0].str);
        context.db->touch(args[0].str);
        return encodeRESPInteger(val);
    } catch (const std::exception&) {
        return encodeRESPError("ERR value is not an integer or out of range");
    }
}

static std::string handleConfig(Context& context, const std::vector<RESPMessage>& args) {
    if (args.size() < 2) return encodeRESPError("ERR wrong number of arguments for 'config'");

    if (toUpper(args[0].str) == "GET") {
        const std::string& param = args[1].str;
        if (param == "dir") {
            return encodeRESPArray({"dir", context.dir});
        }
        if (param == "dbfilename") {
            return encodeRESPArray({"dbfilename", context.dbfilename});
        }
        return encodeRESPArray({});
    }
    return encodeRESPError("ERR unknown subcommand '" + args[0].str + "'");
}

static std::string handleKeys(Context& context, const std::vector<RESPMessage>& args) {
    if (args.size() != 1) return encodeRESPError("ERR wrong number of arguments for 'keys'");

    const std::string& pattern = args[0].str;

    return encodeRESPArray(context.db->keys(pattern));
}

static std::string handleType(Context& context, const std::vector<RESPMessage>& args) {
    if (args.size() != 1) return encodeRESPError("ERR wrong number of arguments for 'type'");

    return encodeRESPSimple(valueTypeName(context.db->typeOf(args[0].str)));
}

void registerBasicCommands(Dispatcher& dispatcher){
    dispatcher.add("PING", handlePing);
    dispatcher.add("ECHO", handleEcho);
    dispatcher.add("SET",  handleSet);
    dispatcher.add("GET",  handleGet);
    dispatcher.add("DEL",  handleDel);
    dispatcher.add("INCR", handleIncr);
    dispatcher.add("CONFIG", handleConfig);
    dispatcher.add("KEYS", handleKeys);
    dispatcher.add("TYPE", handleType);
}
