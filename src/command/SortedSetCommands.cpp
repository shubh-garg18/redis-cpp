#include "command/SortedSetCommands.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "protocol/RESPParser.hpp"
#include "storage/SortedSetStore.hpp"

static std::string wrongTypeError() {
    return encodeRESPError("WRONGTYPE Operation against a key holding the wrong kind of value");
}

static std::string handleZadd(Context& context, const std::vector<RESPMessage>& args) {
    if(args.size()<3 or ((args.size()-1)%2!=0)) return encodeRESPError("ERR wrong number of arguments for 'zadd'");

    const std::string& key=args[0].str;
    if(context.db->hasWrongType(key, ValueType::SortedSet)) return wrongTypeError();

    std::vector<std::pair<std::string, double>> entries;
    for(size_t i=1; i<args.size(); i+=2){
        double score;
        try{
            score=std::stod(args[i].str);
        }
        catch(...)
        {
            return encodeRESPError("ERR value is not a valid float");
        }
        entries.push_back({args[i+1].str, score});
    }

    int count=context.db->sortedSetStore.zadd(key, entries);
    context.db->touch(key);
    return encodeRESPInteger(count);
}

static std::string handleZrank(Context& context, const std::vector<RESPMessage>& args){
    if(args.size()!=2) return encodeRESPError("ERR wrong number of arguments for 'zrank'");

    const std::string& key=args[0].str;
    if(context.db->hasWrongType(key, ValueType::SortedSet)) return wrongTypeError();

    auto rank=context.db->sortedSetStore.zrank(key, args[1].str);
    if(!rank) return encodeRESPNull();
    return encodeRESPInteger(*rank);
}

static std::string handleZrange(Context& context, const std::vector<RESPMessage>& args){
    if(args.size()!=3) return encodeRESPError("ERR wrong number of arguments for 'zrange'");

    const std::string& key=args[0].str;
    if(context.db->hasWrongType(key, ValueType::SortedSet)) return wrongTypeError();

    int start,end;

    try{
        start=(int)parse_int(args[1].str, 0, (int)args[1].str.size());
        end=(int)parse_int(args[2].str, 0, (int)args[2].str.size());
    }
    catch(...)
    {
        return encodeRESPError("ERR value is not an integer or out of range");
    }
    
    auto items=context.db->sortedSetStore.zrange(key, start, end);
    return encodeRESPArray(items);
}


static std::string handleZcard(Context& context, const std::vector<RESPMessage>& args){
    if(args.size()!=1) return encodeRESPError("ERR wrong number of arguments for 'zcard'"); 

    const std::string& key=args[0].str;
    if(context.db->hasWrongType(key, ValueType::SortedSet)) return wrongTypeError();

    return encodeRESPInteger(context.db->sortedSetStore.zcard(key));
}

static std::string handleZscore(Context& context, const std::vector<RESPMessage>& args){
    if(args.size()!=2) return encodeRESPError("ERR wrong number of arguments for 'zscore'");

    const std::string& key=args[0].str;
    if(context.db->hasWrongType(key, ValueType::SortedSet)) return wrongTypeError();

    auto score=context.db->sortedSetStore.zscore(key, args[1].str);
    if(!score) return encodeRESPNull();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", *score);
    return encodeRESPBulk(buf);
}

static std::string handleZrem(Context& context, const std::vector<RESPMessage>& args){
    if(args.size()<2) return encodeRESPError("ERR wrong number of arguments for 'zrem'");

    const std::string& key=args[0].str;
    if(context.db->hasWrongType(key, ValueType::SortedSet)) return wrongTypeError();

    std::vector<std::string> members;
    for(size_t i=1; i<args.size(); i++) members.push_back(args[i].str);
    
    int count=context.db->sortedSetStore.zrem(key, members);
    if(count) context.db->touch(key);
    return encodeRESPInteger(count);
}

void registerSortedSetCommands(Dispatcher& dispatcher) {
    dispatcher.add("ZADD",   handleZadd);
    dispatcher.add("ZRANK",  handleZrank);
    dispatcher.add("ZRANGE", handleZrange);
    dispatcher.add("ZCARD",  handleZcard);
    dispatcher.add("ZSCORE", handleZscore);
    dispatcher.add("ZREM",   handleZrem);
}