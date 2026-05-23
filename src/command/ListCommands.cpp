#include "command/ListCommands.hpp"

#include<string>
#include<vector>

#include "storage/ListStore.hpp"
#include "protocol/RESPParser.hpp"

static std::string handleRpush(Context& context, const std::vector<RESPMessage>& args){
    if(args.size()<2) return encodeRESPError("ERR wrong number of arguments for 'Rpush'");

    const std::string key=args[0].str;
    std::vector<std::string> values;
    for(int i=1; i<args.size(); i++) values.push_back(args[i].str);
    
    int count=context.db->listStore.rpush(key, values);
    return encodeRESPInteger(count);
}

static std::string handleLpush(Context& context, const std::vector<RESPMessage>& args){
    if(args.size()<2) return encodeRESPError("ERR wrong number of arguments for 'Lpush'");

    const std::string key=args[0].str;
    std::vector<std::string> values;
    for(int i=1; i<args.size(); i++) values.push_back(args[i].str);
    
    int count=context.db->listStore.lpush(key, values);
    return encodeRESPInteger(count);
}

static std::string handleLrange(Context& context, const std::vector<RESPMessage>& args){
    if(args.size()!=3) return encodeRESPError("ERR wrong number of arguments for 'Lrange'");

    const std::string key=args[0].str;
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

    std::string res="*"+std::to_string(items.size())+"\r\n";
    for(auto& m: items) res+=encodeRESPBulk(m);

    return res;
}

static std::string handleLen(Context& context, const std::vector<RESPMessage>& args){
    if(args.size()!=1) return encodeRESPError("ERR wrong number of arguments for 'Len'");

    const std::string key=args[0].str;

    int size=context.db->listStore.llen(key);
    return encodeRESPInteger(size);
}

static std::string handleLpop(Context& context, const std::vector<RESPMessage>& args){
    if(args.empty() or args.size()>2) return encodeRESPError("ERR wrong number of arguments for 'lpop'");

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

    auto popped=context.db->listStore.lpop({args[0].str}, count);
    if(popped.empty()) return encodeRESPNull();
    if(args.size()==1) return encodeRESPBulk(popped[0]);

    std::string out="*"+std::to_string(popped.size())+"\r\n";
    for(const auto& s: popped) out+=encodeRESPBulk(s);
    return out;
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
    for(int i=0; i<args.size()-1; i++) keys.push_back(args[i].str);

    auto popped=context.db->listStore.blpop(keys, timeout);
    if(!popped) return "*-1\r\n";

    return "*2\r\n"+encodeRESPBulk(popped->first)+encodeRESPBulk(popped->second);
}

void registerListCommands(Dispatcher& dispatcher){
    dispatcher.add("LPUSH", handleLpush);
    dispatcher.add("RPUSH", handleRpush);
    dispatcher.add("LRANGE", handleLrange);
    dispatcher.add("LLEN", handleLen);
    dispatcher.add("LPOP", handleLpop);
    dispatcher.add("BLPOP", handleBlpop);
}