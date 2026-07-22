#include "command/ListCommands.hpp"

#include<string>
#include<vector>

#include "storage/ListStore.hpp"
#include "protocol/RESPParser.hpp"

static std::string wrongTypeError(){
    return encodeRESPError("WRONGTYPE Operation against a key holding the wrong kind of value");
}

static std::string handleRpush(Context& context, const std::vector<RESPMessage>& args){
    if(args.size()<2) return encodeRESPError("ERR wrong number of arguments for 'rpush'");

    const std::string& key=args[0].str;
    if(context.db->hasWrongType(key, ValueType::List)) return wrongTypeError();

    std::vector<std::string> values;
    for(size_t i=1; i<args.size(); i++) values.push_back(args[i].str);
    
    int count=context.db->listStore.rpush(key, values);
    context.db->touch(key);
    context.db->recordAccess(key);
    return encodeRESPInteger(count);
}

static std::string handleLpush(Context& context, const std::vector<RESPMessage>& args){
    if(args.size()<2) return encodeRESPError("ERR wrong number of arguments for 'lpush'");

    const std::string& key=args[0].str;
    if(context.db->hasWrongType(key, ValueType::List)) return wrongTypeError();

    std::vector<std::string> values;
    for(size_t i=1; i<args.size(); i++) values.push_back(args[i].str);
    
    int count=context.db->listStore.lpush(key, values);
    context.db->touch(key);
    context.db->recordAccess(key);
    return encodeRESPInteger(count);
}

static std::string handleLrange(Context& context, const std::vector<RESPMessage>& args){
    if(args.size()!=3) return encodeRESPError("ERR wrong number of arguments for 'lrange'");

    const std::string& key=args[0].str;
    if(context.db->hasWrongType(key, ValueType::List)) return wrongTypeError();

    int start,end;

    try{
        start=(int)parse_int(args[1].str, 0, (int)args[1].str.size());
        end=(int)parse_int(args[2].str, 0, (int)args[2].str.size());
    }
    catch(...)
    {
        return encodeRESPError("ERR value is not an integer or out of range");
    }
    
    auto items=context.db->listStore.lrange(key, start, end);

    context.db->recordAccess(key);
    return encodeRESPArray(items);
}

static std::string handleLlen(Context& context, const std::vector<RESPMessage>& args){
    if(args.size()!=1) return encodeRESPError("ERR wrong number of arguments for 'llen'");

    const std::string& key=args[0].str;
    if(context.db->hasWrongType(key, ValueType::List)) return wrongTypeError();

    int size=context.db->listStore.llen(key);
    context.db->recordAccess(key);
    return encodeRESPInteger(size);
}

static std::string handleLpop(Context& context, const std::vector<RESPMessage>& args){
    if(args.empty() or args.size()>2) return encodeRESPError("ERR wrong number of arguments for 'lpop'");

    const std::string& key=args[0].str;
    if(context.db->hasWrongType(key, ValueType::List)) return wrongTypeError();

    int count=1;
    if(args.size()==2){
        try{ 
            count=(int)parse_int(args[1].str, 0, (int)args[1].str.size()); 
        }
        catch(...){ 
            return encodeRESPError("ERR value is not an integer or out of range"); 
        }
        
        if(count<0) return encodeRESPError("ERR value is out of range");
    }

    auto popped=context.db->listStore.lpop({key}, count);
    if(popped.empty()) return encodeRESPNull();
    context.db->touch(key);
    context.db->recordAccess(key);
    if(args.size()==1) return encodeRESPBulk(popped[0]);

    return encodeRESPArray(popped);
}

static std::string handleBlpop(Context& context, const std::vector<RESPMessage>& args){
    if(args.size()<2) return encodeRESPError("ERR wrong number of arguments for 'blpop'");

    int timeout;

    try{
        timeout=(int)parse_int(args.back().str, 0, (int)args.back().str.size());
    }
    catch(...)
    {
        return encodeRESPError("ERR value is not an integer or out of range");
    }
    if(timeout<0) return encodeRESPError("ERR value is out of range");

    std::vector<std::string> keys;
    for(size_t i=0; i+1<args.size(); i++){
        if(context.db->hasWrongType(args[i].str, ValueType::List)) return wrongTypeError();
        keys.push_back(args[i].str);
    }

    auto popped=context.db->listStore.blpop(keys, timeout);
    if(!popped) return "*-1\r\n";
    context.db->touch(popped->first);
    context.db->recordAccess(popped->first);
    return encodeRESPArray({popped->first, popped->second});
}

void registerListCommands(Dispatcher& dispatcher){
    dispatcher.add("LPUSH", handleLpush);
    dispatcher.add("RPUSH", handleRpush);
    dispatcher.add("LRANGE", handleLrange);
    dispatcher.add("LLEN", handleLlen);
    dispatcher.add("LPOP", handleLpop);
    dispatcher.add("BLPOP", handleBlpop);
}
