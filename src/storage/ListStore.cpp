#include "storage/ListStore.hpp"

#include<algorithm>
#include<chrono>

int ListStore::rpush(const std::string& key, const std::vector<std::string>& values){
    int size;

    {
        std::lock_guard<std::mutex> lock(mutex);
        auto &v=lists[key];
        for(auto &val: values) v.push_back(val);
        size=(int)v.size();
    }

    cv.notify_all();
    return size;
}

int ListStore::lpush(const std::string& key, const std::vector<std::string>& values){
    int size;

    {
        std::lock_guard<std::mutex> lock(mutex);
        auto &v=lists[key];
        for(auto &val: values) v.insert(v.begin(), val);
        size=(int)v.size();
    }

    cv.notify_all();
    return size;
}

int ListStore::llen(const std::string& key){
    std::lock_guard<std::mutex> lock(mutex);
    
    auto it=lists.find(key);

    if(it==lists.end()) return 0;
    return (int)it->second.size();
}

std::vector<std::string> ListStore::lrange(const std::string& key, int start, int end){
    std::lock_guard<std::mutex> lock(mutex);

    auto it=lists.find(key);
    if(it==lists.end()) return {};

    int size=(int)it->second.size();

    if(start<0) start+=size;
    if(end<0) end+=size;

    if(end>size-1) end=size-1;
    if(start<0) start=0;

    if(start>=size or start>end) return {};

    return std::vector<std::string>(it->second.begin()+start, it->second.begin()+end+1);
}

std::vector<std::string> ListStore::lpop(const std::vector<std::string>& keys, int count){
    std::lock_guard<std::mutex> lock(mutex);

    auto it=lists.find(keys[0]);
    if(it==lists.end() or it->second.empty()) return {};

    if(count<1) count=1;
    int n=std::min(count, (int)it->second.size());

    std::vector<std::string> out;
    for(int i=0; i<n; i++){
        out.push_back(it->second.front()); 
        it->second.erase(it->second.begin());
    }

    if(it->second.empty()) lists.erase(it);

    return out;
}

std::optional<std::pair<std::string, std::string>> 
ListStore::blpop(const std::vector<std::string>& keys, int timeout){
    std::unique_lock<std::mutex> lock(mutex);

    auto has=[&](){
        for(const auto& key: keys){
            auto it=lists.find(key);
            if(it!=lists.end() and !it->second.empty()) return true;
        }
        return false;
    };

    if(!has()){
        if(!timeout) cv.wait(lock, has);
        else if(!cv.wait_for(lock, std::chrono::seconds(timeout), has)) return std::nullopt;
    }

    for(const auto& key: keys){
        auto it=lists.find(key);
        if(it!=lists.end() and !it->second.empty()){
            std::string val=it->second.front();
            it->second.erase(it->second.begin());
            if(it->second.empty()) lists.erase(it);
            return std::make_pair(key, val);
        }
    }

    return std::nullopt;
}

bool ListStore::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex);
    return lists.erase(key) > 0;
}

bool ListStore::exists(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = lists.find(key);
    return it != lists.end() && !it->second.empty();
}

std::vector<std::string> ListStore::keys() {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::string> res;
    for (const auto& [k, v] : lists) {
        if (!v.empty()) res.push_back(k);
    }
    return res;
}