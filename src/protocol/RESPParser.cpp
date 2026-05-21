#include "protocol/RESPParser.hpp"

#include<cstdint>
#include <stdexcept>

// finds the next \r\n in chunk starting from `from`. -1 if not found.
static int find_crlf(const std::string &chunk, int from){
    for(int i=from;i+1<(int)chunk.size();i++){
        if(chunk[i]=='\r' and chunk[i+1]=='\n'){
            return i;
        }
    }
    return -1;
}

// parses an integer from chunk[start..end). throws on garbage.
int64_t parse_int(const std::string &chunk, int start, int end){
    if(start>=end) throw std::runtime_error("RESP: empty integer");
    int i=start;
    bool neg=false;
    if(chunk[i]=='-'){
        neg=true;
        i++;
    }
    int64_t res=0;
    bool exist=false;
    while(i<end){
        char c=chunk[i];
        if(c<'0' or c>'9') throw std::runtime_error("RESP: invalid integer");
        res=res*10+(c-'0');
        exist=true;
        i++;
    }
    if(!exist) throw std::runtime_error("RESP: empty integer");
    return neg?-res:res;
}

static ParseResult ParseAt(const std::string& chunk, int offset);

// Example: "+OK\r\n" or "-ERR ...\r\n"
static ParseResult stringParser(const std::string &chunk, int offset, RESPMessage::Type type){
    int end=find_crlf(chunk, offset+1);
    if(end==-1) return {};
    RESPMessage msg;
    msg.type=type;
    msg.str=chunk.substr(offset+1, end-offset-1);
    ParseResult res;
    res.value=msg;
    res.len=end+2-offset;
    res.ok=true;
    return res;
}

// Example: ":123\r\n"
static ParseResult integerParser(const std::string &chunk, int offset){
    int end=find_crlf(chunk, offset+1);
    if(end==-1) return {};
    RESPMessage msg;
    msg.type=RESPMessage::Type::INT;
    msg.n=parse_int(chunk, offset+1, end);
    ParseResult res;
    res.value=msg;
    res.len=end+2-offset;
    res.ok=true;
    return res;
}

// Example: "$5\r\nhello\r\n"
static ParseResult bulkStringParser(const std::string &chunk, int offset){
    int end=find_crlf(chunk, offset+1);
    if(end==-1) return {};

    int64_t len=parse_int(chunk, offset+1, end);

    RESPMessage msg;
    if(len==-1){
        msg.type=RESPMessage::Type::NIL;
        ParseResult res;
        res.value=msg;
        res.len=end+2-offset;
        res.ok=true;
        return res;
    }

    if((int)chunk.size()<end+2+len+2) return {};

    if(chunk[end+2+len]!='\r' or chunk[end+2+len+1]!='\n') 
        throw std::runtime_error("RESP: invalid bulk string");

    msg.type=RESPMessage::Type::BULK;
    msg.str=chunk.substr(end+2, len);

    ParseResult res;
    res.value=msg;
    res.len=end+2+len+2-offset;
    res.ok=true;
    return res;
}

// Example: "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n"
static ParseResult arrayParser(const std::string &chunk, int offset){
    int end=find_crlf(chunk, offset+1);
    if(end==-1) return {};

    int64_t len=parse_int(chunk, offset+1, end);

    RESPMessage msg;
    int cursor=end+2;
    if(len==-1){
        msg.type=RESPMessage::Type::NIL;
        ParseResult res;
        res.value=msg;
        res.len=cursor-offset;
        res.ok=true;
        return res;
    }

    msg.type=RESPMessage::Type::ARR;
    msg.arr.reserve(len);

    for(int i=0;i<len;i++){
        ParseResult child=ParseAt(chunk, cursor);
        if(!child.ok) return {};
        msg.arr.push_back(child.value);
        cursor+=child.len;
    }

    ParseResult res;
    res.value=msg;
    res.len=cursor-offset;
    res.ok=true;
    return res;
}

static ParseResult ParseAt(const std::string &chunk, int offset){
    if(offset>=(int)chunk.size()) return {};

    char pref=chunk[offset];

    switch(pref){
        case '+':
            return stringParser(chunk, offset, RESPMessage::Type::STR);
        case '-':
            return stringParser(chunk, offset, RESPMessage::Type::ERR);
        case ':':
            return integerParser(chunk, offset);
        case '$':
            return bulkStringParser(chunk, offset);
        case '*':
            return arrayParser(chunk, offset);
        default:
            throw std::runtime_error("RESP: invalid prefix");
    }
}

ParseResult RESPParser(const std::string& chunk){
    return ParseAt(chunk, 0);
}

std::string encodeRESPSimple(const std::string& s){
    return "+"+s+"\r\n";
}

std::string encodeRESPError(const std::string& s){
    return "-"+s+"\r\n";
}

std::string encodeRESPInteger(long long n){
    return ":"+std::to_string(n)+"\r\n";
}

std::string encodeRESPBulk(const std::string& s){
    return "$"+std::to_string(s.size())+"\r\n"+s+"\r\n";
}

std::string encodeRESPNull(){
    return "$-1\r\n";
}