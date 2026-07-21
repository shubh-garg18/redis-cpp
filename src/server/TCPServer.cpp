/*
1. Listening socket vs connection socket. One fd waits for new clients (listen_fd); each accept() returns a brand new fd for that specific conversation. 
The listener stays alive forever; the connection fds come and go.

2. The accept loop pattern. while(true) { accept; spawn worker; }. The listener thread does almost nothing except wait in accept(). 
All real work happens in workers.

3. Concurrency model: thread-per-connection. Simple, scales to ~hundreds of clients, falls over around ~10k (the "C10K problem"). 
Real Redis uses a single-threaded event loop instead. 

Thread-per-connection: easy to reason about, OS schedules for you, costs ~MB of stack per thread.
Event loop (epoll/kqueue): handles 100k+ connections in one thread, but every handler must be non-blocking.
*/

#include "server/TCPServer.hpp"
#include "server/ClientSession.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

TCPServer::TCPServer(int p, Context* ctx, Dispatcher* d)
    : port(p), context(ctx), dispatcher(d) {}

void TCPServer::start(){
    int listen_fd=::socket(AF_INET, SOCK_STREAM, 0);
    if(listen_fd<0){
        throw std::runtime_error(std::string("socket: ")+std::strerror(errno));
    }

    // allow rebind right after restart instead of waiting for TIME_WAIT
    int yes=1;
    if(::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes))<0){
        ::close(listen_fd);
        throw std::runtime_error(std::string("setsockopt: ")+std::strerror(errno));
    }

    sockaddr_in addr{};
    addr.sin_family=AF_INET;
    addr.sin_addr.s_addr=htonl(INADDR_ANY);
    addr.sin_port=htons((uint16_t)port);

    if(::bind(listen_fd, (sockaddr*)&addr, sizeof(addr))<0){
        ::close(listen_fd);
        throw std::runtime_error(std::string("bind: ")+std::strerror(errno));
    }

    if(::listen(listen_fd, 128)<0){
        ::close(listen_fd);
        throw std::runtime_error(std::string("listen: ")+std::strerror(errno));
    }

    std::cout << "Redis-like server running on port " << port << "\n";

    while(true){
        sockaddr_in client{};
        socklen_t len=sizeof(client);
        int connection=::accept(listen_fd, (sockaddr*)&client, &len);
        if(connection<0){
            if(errno==EINTR) continue;
            std::cerr<<"accept: "<<std::strerror(errno)<<"\n";
            continue;
        }

        // Disable Nagle: replies are small and request/response, so batching
        // them behind the ACK timer just adds latency (and stalls pipelines).
        int nodelay=1;
        ::setsockopt(connection, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        Context* ctx = context;
        Dispatcher* disp = dispatcher;
        std::thread t([connection, ctx, disp]() {
            try {
                handle_client(connection, ctx, *disp);
            } catch (const std::exception& e) {
                std::cerr << "client error: " << e.what() << "\n";
            }
        });
        t.detach();
    }
}
