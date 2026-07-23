#include "server/ReplicaLink.hpp"
#include "protocol/RESPParser.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

static bool sendAll(int fd, const std::string& data){
    const char* p=data.data();
    size_t left=data.size();
    while(left>0){
        ssize_t n=::write(fd, p, left);
        if(n<0){ if(errno==EINTR) continue; return false; }
        if(n==0) return false;
        p+=n; left-=(size_t)n;
    }
    return true;
}

// Read until the buffer holds a CRLF, hand back the line before it, and drop the
// line (+CRLF) from the buffer. Bytes read past the CRLF stay buffered for the
// next step — which matters because the snapshot follows +FULLRESYNC with no gap.
static bool readLine(int fd, std::string& buffer, std::string& line){
    while(true){
        size_t pos=buffer.find("\r\n");
        if(pos!=std::string::npos){
            line=buffer.substr(0, pos);
            buffer.erase(0, pos+2);
            return true;
        }
        char chunk[4096];
        ssize_t n=::read(fd, chunk, sizeof(chunk));
        if(n<=0){ if(n<0 && errno==EINTR) continue; return false; }
        buffer.append(chunk, (size_t)n);
    }
}

// Connect to the master, retrying because it may boot after the replica.
static int dialMaster(const std::string& host, int port){
    for(int attempt=0; attempt<30; attempt++){
        int fd=::socket(AF_INET, SOCK_STREAM, 0);
        if(fd<0) return -1;
        sockaddr_in addr{};
        addr.sin_family=AF_INET;
        addr.sin_port=htons((uint16_t)port);
        if(::inet_pton(AF_INET, host.c_str(), &addr.sin_addr)<=0){ ::close(fd); return -1; }
        if(::connect(fd, (sockaddr*)&addr, sizeof(addr))==0){
            int nodelay=1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
            return fd;
        }
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return -1;
}

void connectToMaster(const std::string& host, int port, int ownPort,
                     Dispatcher& dispatcher, Context& ctx){
    int fd=dialMaster(host, port);
    if(fd<0){ std::cerr<<"replica: cannot reach master "<<host<<":"<<port<<"\n"; return; }

    std::string buffer, line;

    // Handshake: read each reply before sending the next command.
    if(!sendAll(fd, encodeRESPArray({"PING"})) || !readLine(fd, buffer, line)){ ::close(fd); return; }
    if(!sendAll(fd, encodeRESPArray({"REPLCONF","listening-port",std::to_string(ownPort)})) || !readLine(fd, buffer, line)){ ::close(fd); return; }
    if(!sendAll(fd, encodeRESPArray({"REPLCONF","capa","psync2"})) || !readLine(fd, buffer, line)){ ::close(fd); return; }
    if(!sendAll(fd, encodeRESPArray({"PSYNC","?","-1"})) || !readLine(fd, buffer, line)){ ::close(fd); return; }
    // line == "+FULLRESYNC <replid> <offset>"; the snapshot + live stream follow.

    // Drain loop: parse each RESP array and apply it through the dispatcher. This
    // ctx has client == null, so the READONLY gate lets master writes through.
    while(true){
        while(!buffer.empty()){
            ParseResult r;
            try { r=RESPParser(buffer); }
            catch(const std::exception&){ ::close(fd); return; }
            if(!r.ok) break;                     // incomplete: read more bytes
            if(r.value.type==RESPMessage::Type::ARR && !r.value.arr.empty()){
                const std::string& cmd=r.value.arr[0].str;
                std::vector<RESPMessage> args(r.value.arr.begin()+1, r.value.arr.end());
                dispatcher.executeCommand(ctx, cmd, args);
            }
            buffer.erase(0, (size_t)r.len);
        }
        char chunk[16384];
        ssize_t n=::read(fd, chunk, sizeof(chunk));
        if(n<=0){ if(n<0 && errno==EINTR) continue; break; }   // master gone: stop
        buffer.append(chunk, (size_t)n);
    }
    ::close(fd);
}
