#include "repl/ReplState.hpp"
#include "server/ClientState.hpp"

#include <unistd.h>
#include <cerrno>

// Push bytes to one replica's socket, serialized with that connection's own
// writes. Same primitive as PubSub::writeToClient — a replica connection can be
// written to both by its handler thread and by any thread propagating a write.
static void writeToReplica(ClientState* c, const std::string& data){
    std::lock_guard<std::mutex> lock(c->writeMutex);
    const char* p=data.data();
    size_t left=data.size();
    while(left>0){
        ssize_t n=::write(c->fd, p, left);
        if(n<0){
            if(errno==EINTR) continue;
            break;
        }
        if(n==0) break;
        p+=n;
        left-=(size_t)n;
    }
}

void ReplState::syncReplica(ClientState* c, const std::function<std::string()>& buildSnapshot){
    std::lock_guard<std::mutex> lock(mutex);
    std::string snapshot=buildSnapshot();   // built under the lock: no propagate() can interleave
    writeToReplica(c, snapshot);            // snapshot goes out first...
    replicas.insert(c);                     // ...then future propagate()s reach it
}

void ReplState::dropReplica(ClientState* c){
    std::lock_guard<std::mutex> lock(mutex);
    replicas.erase(c);
}

void ReplState::propagate(const std::string& frame){
    std::lock_guard<std::mutex> lock(mutex);
    for(ClientState* c : replicas) writeToReplica(c, frame);
}

int ReplState::replicaCount(){
    std::lock_guard<std::mutex> lock(mutex);
    return (int)replicas.size();
}
