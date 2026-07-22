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

static ParseResult incompleteResult() {
    ParseResult r;
    r.ok=false;
    r.incomplete=true;
    return r;
}

static ParseResult successResult(RESPMessage v, int consumed) {
    ParseResult r;
    r.value=std::move(v);
    r.len=consumed;
    r.ok=true;
    r.incomplete=false;
    return r;
}

static ParseResult ParseAt(const std::string& chunk, int offset);

// Example: "+OK\r\n" or "-ERR ...\r\n"
static ParseResult stringParser(const std::string &chunk, int offset, RESPMessage::Type type){
    int end=find_crlf(chunk, offset+1);
    if(end==-1) return incompleteResult();
    RESPMessage msg;
    msg.type=type;
    msg.str=chunk.substr(offset+1, end-offset-1);
    return successResult(msg, end+2-offset);
}

// Example: ":123\r\n"
static ParseResult integerParser(const std::string &chunk, int offset){
    int end=find_crlf(chunk, offset+1);
    if(end==-1) return incompleteResult();
    RESPMessage msg;
    msg.type=RESPMessage::Type::INT;
    msg.n=parse_int(chunk, offset+1, end);
    return successResult(msg, end+2-offset);
}

// Example: "$5\r\nhello\r\n"
static ParseResult bulkStringParser(const std::string &chunk, int offset){
    int end=find_crlf(chunk, offset+1);
    if(end==-1) return incompleteResult();

    int64_t len=parse_int(chunk, offset+1, end);
    int64_t cursor=end+2;

    RESPMessage msg;
    if(len==-1){
        msg.type=RESPMessage::Type::NIL;
        return successResult(msg, (int)(cursor-offset));
    }

    if((int64_t)chunk.size()<cursor+len+2) return incompleteResult();

    if(chunk[cursor+len]!='\r' or chunk[cursor+len+1]!='\n') 
        throw std::runtime_error("RESP: invalid bulk string");

    msg.type=RESPMessage::Type::BULK;
    msg.str=chunk.substr(cursor, len);

    return successResult(msg, (int)(cursor+len+2-offset));
}

// Example: "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n"
static ParseResult arrayParser(const std::string &chunk, int offset){
    int end=find_crlf(chunk, offset+1);
    if(end==-1) return incompleteResult();

    int64_t len=parse_int(chunk, offset+1, end);

    RESPMessage msg;
    int64_t cursor=end+2;

    if(len==-1){
        msg.type=RESPMessage::Type::NIL;
        return successResult(msg, (int)(cursor-offset));
    }

    msg.type=RESPMessage::Type::ARR;
    msg.arr.reserve(len);

    for(int i=0;i<len;i++){
        ParseResult child=ParseAt(chunk, (int)cursor);
        if(!child.ok){
            if(child.incomplete) return incompleteResult();
            throw std::runtime_error("RESP: array child parse failed");
        }
        msg.arr.push_back(std::move(child.value));
        cursor+=child.len;
    }

    return successResult(msg, (int)(cursor-offset));
}

static ParseResult ParseAt(const std::string &chunk, int offset){
    if(offset>=(int)chunk.size()) return incompleteResult();

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
            throw std::runtime_error("RESP: unknown type byte");
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

std::string encodeRESPInteger(int64_t n){
    return ":"+std::to_string(n)+"\r\n";
}

std::string encodeRESPBulk(const std::string& s){
    return "$"+std::to_string(s.size())+"\r\n"+s+"\r\n";
}

std::string encodeRESPNull(){
    return "$-1\r\n";
}

std::string encodeRESPArray(const std::vector<std::string>& items) {
    std::string out = encodeRESPArrayHeader(items.size());
    for (const auto& item : items) out += encodeRESPBulk(item);
    return out;
}

std::string encodeRESPArrayHeader(size_t size) {
    return "*" + std::to_string(size) + "\r\n";
}