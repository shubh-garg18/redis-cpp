#include "storage/StreamStore.hpp"

#include<chrono>

static int64_t now_ms(){
    auto now=std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

XAddResult StreamStore::xadd(const std::string& key, int64_t ms, int64_t seq,
                             const std::vector<std::string>& fields){
    XAddResult res;

    {
        std::lock_guard<std::mutex> lock(mutex);

        auto it=streams.find(key);
        bool fresh=(it==streams.end());
        StreamID top=fresh ? StreamID{} : it->second.top;

        StreamID id;
        if(ms<0){
            int64_t t=now_ms();
            if((uint64_t)t>top.ms){
                id.ms=(uint64_t)t;
                id.seq=0;
            }
            else{
                // clock stood still or went backwards, keep climbing on seq
                id.ms=top.ms;
                id.seq=top.seq+1;
            }
        }
        else{
            id.ms=(uint64_t)ms;
            if(seq<0) id.seq=(id.ms==top.ms) ? top.seq+1 : 0;
            else id.seq=(uint64_t)seq;
        }

        if(id.ms==0 and id.seq==0){
            res.error="ERR The ID specified in XADD must be greater than 0-0";
            return res;
        }
        if(!fresh and id<=top){
            res.error="ERR The ID specified in XADD is equal or smaller than the target stream top item";
            return res;
        }

        auto& s=streams[key];
        s.entries.push_back({id, fields});
        s.top=id;

        res.ok=true;
        res.id=id;
    }

    cv.notify_all();
    return res;
}

std::vector<StreamEntry> StreamStore::xrange(const std::string& key, StreamID start, StreamID end){
    std::lock_guard<std::mutex> lock(mutex);

    auto it=streams.find(key);
    if(it==streams.end()) return {};

    std::vector<StreamEntry> out;
    for(const auto& e: it->second.entries){
        if(end<e.id) break;
        if(start<=e.id) out.push_back(e);
    }
    return out;
}

std::vector<std::pair<std::string, std::vector<StreamEntry>>>
StreamStore::collect(const std::vector<std::pair<std::string, StreamID>>& queries){
    std::vector<std::pair<std::string, std::vector<StreamEntry>>> out;

    for(const auto& [key, after] : queries){
        auto it=streams.find(key);
        if(it==streams.end()) continue;

        std::vector<StreamEntry> hits;
        for(const auto& e: it->second.entries){
            if(after<e.id) hits.push_back(e);
        }

        if(!hits.empty()) out.push_back({key, hits});
    }
    return out;
}

std::vector<std::pair<std::string, std::vector<StreamEntry>>>
StreamStore::xread(const std::vector<std::pair<std::string, StreamID>>& queries, int64_t block_ms){
    std::unique_lock<std::mutex> lock(mutex);

    auto out=collect(queries);
    if(!out.empty() or block_ms<0) return out;

    auto ready=[&](){
        out=collect(queries);
        return !out.empty();
    };

    if(!block_ms) cv.wait(lock, ready);
    else cv.wait_for(lock, std::chrono::milliseconds(block_ms), ready);

    return out;
}

StreamID StreamStore::lastId(const std::string& key){
    std::lock_guard<std::mutex> lock(mutex);

    auto it=streams.find(key);
    if(it==streams.end()) return {};
    return it->second.top;
}

bool StreamStore::del(const std::string& key){
    std::lock_guard<std::mutex> lock(mutex);
    return streams.erase(key)>0;
}

bool StreamStore::exists(const std::string& key){
    std::lock_guard<std::mutex> lock(mutex);
    return streams.count(key)>0;
}

std::vector<std::string> StreamStore::keys(){
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::string> res;
    for(const auto& [key, _] : streams) res.push_back(key);
    return res;
}
