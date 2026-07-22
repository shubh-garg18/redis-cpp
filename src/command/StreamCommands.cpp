#include "command/StreamCommands.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "protocol/RESPParser.hpp"
#include "storage/StreamStore.hpp"

static std::string wrongTypeError() {
    return encodeRESPError("WRONGTYPE Operation against a key holding the wrong kind of value");
}

// "*" -> -1,-1   "5-*" -> 5,-1   "5" -> 5,-1   "5-3" -> 5,3
static bool parseXaddId(const std::string& s, int64_t& ms, int64_t& seq){
    if(s=="*"){ ms=-1; seq=-1; return true; }
    auto dash=s.find('-');
    try{
        if(dash==std::string::npos){
            ms=(int64_t)std::stoll(s);
            seq=-1;
            return ms>=0;
        }
        ms=(int64_t)std::stoll(s.substr(0, dash));
        std::string b=s.substr(dash+1);
        if(b=="*") seq=-1;
        else seq=(int64_t)std::stoll(b);
        return ms>=0 and seq>=-1;
    }catch(...){ return false; }
}

// XRANGE bounds. "-" is the smallest id, "+" the largest. A bare "5" fills the
// missing seq with 0 for the start and the max seq for the end.
static bool parseRangeId(const std::string& s, bool isStart, StreamID& out){
    if(s=="-"){ out={0,0}; return true; }
    if(s=="+"){ out={UINT64_MAX, UINT64_MAX}; return true; }
    auto dash=s.find('-');
    try{
        if(dash==std::string::npos){
            out.ms=(uint64_t)std::stoull(s);
            out.seq=isStart ? 0 : UINT64_MAX;
            return true;
        }
        out.ms=(uint64_t)std::stoull(s.substr(0, dash));
        out.seq=(uint64_t)std::stoull(s.substr(dash+1));
        return true;
    }catch(...){ return false; }
}

// XREAD cursor id. A bare "5" means 5-0. "$" is resolved by the caller.
static bool parseReadId(const std::string& s, StreamID& out){
    auto dash=s.find('-');
    try{
        if(dash==std::string::npos){
            out.ms=(uint64_t)std::stoull(s);
            out.seq=0;
            return true;
        }
        out.ms=(uint64_t)std::stoull(s.substr(0, dash));
        out.seq=(uint64_t)std::stoull(s.substr(dash+1));
        return true;
    }catch(...){ return false; }
}

static std::string encodeEntry(const StreamEntry& e){
    std::string s=encodeRESPArrayHeader(2);
    s+=encodeRESPBulk(e.id.toString());
    s+=encodeRESPArrayHeader(e.fields.size());
    for(const auto& f: e.fields) s+=encodeRESPBulk(f);
    return s;
}

static std::string handleXadd(Context& context, const std::vector<RESPMessage>& args){
    if(args.size()<4 or args.size()%2!=0) return encodeRESPError("ERR wrong number of arguments for 'xadd'");

    const std::string& key=args[0].str;
    if(context.db->hasWrongType(key, ValueType::Stream)) return wrongTypeError();

    int64_t ms, seq;
    if(!parseXaddId(args[1].str, ms, seq)) return encodeRESPError("ERR Invalid stream ID specified as stream command argument");

    std::vector<std::string> fields;
    for(size_t i=2; i<args.size(); i++) fields.push_back(args[i].str);

    XAddResult res=context.db->streamStore.xadd(key, ms, seq, fields);
    if(!res.ok) return encodeRESPError(res.error);

    context.db->touch(key);
    context.db->recordAccess(key);
    return encodeRESPBulk(res.id.toString());
}

static std::string handleXrange(Context& context, const std::vector<RESPMessage>& args){
    if(args.size()!=3) return encodeRESPError("ERR wrong number of arguments for 'xrange'");

    const std::string& key=args[0].str;
    if(context.db->hasWrongType(key, ValueType::Stream)) return wrongTypeError();

    StreamID start, end;
    if(!parseRangeId(args[1].str, true, start) or !parseRangeId(args[2].str, false, end))
        return encodeRESPError("ERR Invalid stream ID specified as stream command argument");

    auto entries=context.db->streamStore.xrange(key, start, end);

    context.db->recordAccess(key);
    std::string reply=encodeRESPArrayHeader(entries.size());
    for(const auto& e: entries) reply+=encodeEntry(e);
    return reply;
}

static std::string handleXread(Context& context, const std::vector<RESPMessage>& args){
    size_t i=0;
    int64_t block=-1;
    int64_t count=-1;

    // BLOCK and COUNT may come in any order, up until the STREAMS keyword
    while(i<args.size()){
        std::string opt=toUpper(args[i].str);
        if(opt=="BLOCK"){
            if(i+1>=args.size()) return encodeRESPError("ERR syntax error");
            try{ block=(int64_t)std::stoll(args[i+1].str); }
            catch(...){ return encodeRESPError("ERR timeout is not an integer or out of range"); }
            if(block<0) return encodeRESPError("ERR timeout is negative");
            i+=2;
        }
        else if(opt=="COUNT"){
            if(i+1>=args.size()) return encodeRESPError("ERR syntax error");
            try{ count=(int64_t)std::stoll(args[i+1].str); }
            catch(...){ return encodeRESPError("ERR value is not an integer or out of range"); }
            i+=2;
        }
        else break;
    }

    if(i>=args.size() or toUpper(args[i].str)!="STREAMS") return encodeRESPError("ERR syntax error");
    i++;

    size_t rest=args.size()-i;
    if(rest==0 or rest%2!=0) return encodeRESPError("ERR Unbalanced XREAD list of streams: for each stream key an ID or '$' must be specified.");
    size_t nkeys=rest/2;

    for(size_t k=0; k<nkeys; k++){
        if(context.db->hasWrongType(args[i+k].str, ValueType::Stream)) return wrongTypeError();
    }

    std::vector<std::pair<std::string, StreamID>> queries;
    for(size_t k=0; k<nkeys; k++){
        const std::string& key=args[i+k].str;
        const std::string& idstr=args[i+nkeys+k].str;

        StreamID id;
        if(idstr=="$"){
            id=context.db->streamStore.lastId(key);
        }
        else if(!parseReadId(idstr, id)){
            return encodeRESPError("ERR Invalid stream ID specified as stream command argument");
        }
        queries.push_back({key, id});
    }

    auto result=context.db->streamStore.xread(queries, block);
    if(result.empty()) return "*-1\r\n";  // nil array: nothing to read / timed out

    std::string reply=encodeRESPArrayHeader(result.size());
    for(const auto& [key, entries]: result){
        context.db->recordAccess(key);
        size_t n=entries.size();
        if(count>0 and (size_t)count<n) n=(size_t)count;

        reply+=encodeRESPArrayHeader(2);
        reply+=encodeRESPBulk(key);
        reply+=encodeRESPArrayHeader(n);
        for(size_t e=0; e<n; e++) reply+=encodeEntry(entries[e]);
    }
    return reply;
}

void registerStreamCommands(Dispatcher& dispatcher){
    dispatcher.add("XADD",   handleXadd);
    dispatcher.add("XRANGE", handleXrange);
    dispatcher.add("XREAD",  handleXread);
}
