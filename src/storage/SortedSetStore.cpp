#include "storage/SortedSetStore.hpp"

int SortedSetStore::zadd(const std::string& key, const std::vector<std::pair<std::string, double>>& entries) {
    std::lock_guard<std::mutex> lock(mutex);
    auto &set=sets[key];

    int count=0;

    for(const auto& [member, score] : entries){
        auto it=set.scores.find(member);
        if(it==set.scores.end()){
            set.scores[member]=score;
            count++;
        }   
        else{
            set.sorted.erase({it->second, member});
            it->second=score;
        }

        set.sorted.insert({score, member});
    }

    return count;
}

std::optional<int> SortedSetStore::zrank(const std::string& key, const std::string& member){
    std::lock_guard<std::mutex> lock(mutex);

    auto set=sets.find(key);
    if(set==sets.end()) return std::nullopt;

    auto it=set->second.scores.find(member);
    if(it==set->second.scores.end()) return std::nullopt;

    auto rank=std::distance(set->second.sorted.begin(), set->second.sorted.find({it->second, member}));
    return rank;
}

std::vector<std::string> SortedSetStore::zrange(const std::string& key, int start, int end){
    std::lock_guard<std::mutex> lock(mutex);

    auto set=sets.find(key);
    if(set==sets.end()) return {};

    int size=(int)set->second.scores.size();

    if(start<0) start+=size;
    if(end<0) end+=size;
    if(end>size-1) end=size-1;
    if(start<0) start=0;

    if(start>=size or start>end) return {};

    std::vector<std::string> res;
    auto it=set->second.sorted.begin();
    std::advance(it, start);

    for(int i=start; i<=end; i++, it++) res.push_back(it->second);
    return res;
}

int SortedSetStore::zcard(const std::string& key){    
    std::lock_guard<std::mutex> lock(mutex);
    
    auto set=sets.find(key);
    if(set==sets.end()) return 0;
    
    return (int)set->second.scores.size();
}

std::optional<double> SortedSetStore::zscore(const std::string& key, const std::string& member){
    std::lock_guard<std::mutex> lock(mutex);
    
    auto set=sets.find(key);
    if(set==sets.end()) return std::nullopt;
    
    auto it=set->second.scores.find(member);
    if(it==set->second.scores.end()) return std::nullopt;
    
    return it->second;
}

int SortedSetStore::zrem(const std::string& key, const std::vector<std::string>& members){
    std::lock_guard<std::mutex> lock(mutex);
    
    auto set=sets.find(key);
    if(set==sets.end()) return 0;
    
    auto& zs=set->second;
    int count=0;
    
    for(const auto& member : members){
        auto it=zs.scores.find(member);
        if(it==zs.scores.end()) continue;
        
        zs.sorted.erase({it->second, member});
        zs.scores.erase(it);
        count++;
    }

    if(zs.scores.empty()) sets.erase(set);
    return count;
}

bool SortedSetStore::del(const std::string& key){
    std::lock_guard<std::mutex> lock(mutex);
    return sets.erase(key)>0;
}

bool SortedSetStore::exists(const std::string& key){
    std::lock_guard<std::mutex> lock(mutex);
    return sets.count(key)>0;
}

std::vector<std::string> SortedSetStore::keys(){
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::string> res;
    for(const auto& [key, _] : sets) res.push_back(key);
    return res;
}