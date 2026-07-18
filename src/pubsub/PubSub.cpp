#include "pubsub/PubSub.hpp"
#include "server/ClientState.hpp"
#include "protocol/RESPParser.hpp"

#include <unistd.h>
#include <cerrno>

// Push bytes to one subscriber, serialized with that client's own replies.
static void writeToClient(ClientState* c, const std::string& data){
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

void PubSub::subscribe(ClientState* c, const std::string& channel){
    std::lock_guard<std::mutex> lock(mutex);
    channelSubs[channel].insert(c);
    c->channels.insert(channel);
}

void PubSub::unsubscribe(ClientState* c, const std::string& channel){
    std::lock_guard<std::mutex> lock(mutex);
    auto it=channelSubs.find(channel);
    if(it!=channelSubs.end()){
        it->second.erase(c);
        if(it->second.empty()) channelSubs.erase(it);
    }
    c->channels.erase(channel);
}

void PubSub::dropClient(ClientState* c){
    std::lock_guard<std::mutex> lock(mutex);
    for(const auto& channel : c->channels){
        auto it=channelSubs.find(channel);
        if(it!=channelSubs.end()){
            it->second.erase(c);
            if(it->second.empty()) channelSubs.erase(it);
        }
    }
    c->channels.clear();
}

int PubSub::publish(const std::string& channel, const std::string& message){
    std::string frame=encodeRESPArray({"message", channel, message});

    std::lock_guard<std::mutex> lock(mutex);
    auto it=channelSubs.find(channel);
    if(it==channelSubs.end()) return 0;

    int delivered=0;
    for(ClientState* c : it->second){
        writeToClient(c, frame);
        delivered++;
    }
    return delivered;
}

std::vector<std::string> PubSub::listChannels(){
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::string> out;
    for(const auto& entry : channelSubs){
        if(!entry.second.empty()) out.push_back(entry.first);
    }
    return out;
}

int PubSub::numSubscribers(const std::string& channel){
    std::lock_guard<std::mutex> lock(mutex);
    auto it=channelSubs.find(channel);
    if(it==channelSubs.end()) return 0;
    return (int)it->second.size();
}
