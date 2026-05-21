/*
forever:
    bytes = read(socket)
    if EOF/error: stop
    append bytes to buffer

    forever:
        try to parse one frame from buffer
        if incomplete: go read more bytes
        if junk: send error, stop
        dispatch frame -> reply
        send reply
        chop consumed bytes off buffer
        if buffer empty: go read more

close(socket)
*/

/*
1. TCP is a byte stream, not a message stream. One read() can give us half a
command, a whole command, or several pipelined commands at once. We keep a
cumulative buffer across reads and hand it to the RESP parser, which reports
how many bytes formed one complete frame.

2. Two-loop structure. The outer loop calls read() to pull more bytes from the
socket. The inner loop drains every complete frame already sitting in the
buffer (handles pipelined commands) before going back to read.

3. Partial writes. write() is allowed to transfer fewer bytes than requested
when the kernel send buffer is nearly full. write_all() loops over the
remaining tail until the whole reply has been delivered.

4. Shared state across threads. TCPServer spawns one thread per client, so
many copies of handle_client run in parallel. The Database and Dispatcher are
shared; concurrent SET/GET/DEL is safe because StringStore guards its map
with a std::mutex.
*/

#include "server/ClientSession.hpp"
#include "protocol/RESPParser.hpp"

#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

static bool write_all(int connection, const std::string& data){
    const char* p=data.data();
    size_t left=data.size();
    while(left>0){
        ssize_t n=::write(connection, p, left);
        if(n<0){
            if(errno==EINTR) continue;
            return false;
        }
        if(n==0) return false;
        p+=n;
        left-=(size_t)n;
    }
    return true;
}

void handle_client(int connection, Context* context, Dispatcher& dispatcher){
    std::string buffer;
    char chunk[4096];
    bool running=true;

    while(running){
        ssize_t n=::read(connection, chunk, sizeof(chunk));
        if(n==0) break;
        if(n<0){
            if(errno==EINTR) continue;
            std::cerr<<"read error: "<<std::strerror(errno)<<"\n";
            break;
        }
        buffer.append(chunk, (size_t)n);

        while(running){
            ParseResult r;
            try {
                r=RESPParser(buffer);
            } catch(const std::exception& e){
                write_all(connection, encodeRESPError(std::string("ERR protocol: ")+e.what()));
                running=false;
                break;
            }

            if(!r.ok){
                if(r.incomplete) break;
                running=false;
                break;
            }

            std::string reply;
            if(r.value.type!=RESPMessage::Type::ARR || r.value.arr.empty()){
                reply=encodeRESPError("ERR expected array of bulk strings");
            } else {
                const std::string& cmd=r.value.arr[0].str;
                std::vector<RESPMessage> args(r.value.arr.begin()+1, r.value.arr.end());
                reply=dispatcher.executeCommand(*context, cmd, args);
            }
            if(!write_all(connection, reply)){
                running=false;
                break;
            }

            buffer.erase(0, (size_t)r.len);
            if(buffer.empty()) break;
        }
    }

    ::close(connection);
}
