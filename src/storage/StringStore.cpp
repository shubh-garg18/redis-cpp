#include "storage/StringStore.hpp"

int64_t StringStore::Date_now(){
    using namespace std::chrono;
    auto now = steady_clock::now();
    auto ms =
        duration_cast<milliseconds>(
            now.time_since_epoch()
        ).count();
    return ms;
}

void StringStore::set(const std::string& key, const std::string& value, int64_t expiry){
    /*
        lock
        insert/overwrite
        unlock
    */
    Entry entry;
    entry.value=value;
    entry.expiry=expiry;

    std::lock_guard<std::mutex> lock(mutex);

    setMap[key]=entry;
}

std::optional<std::string> StringStore::get(const std::string& key){
    /*
        lock
        find key

        if missing:
            return false

        if expired:
            erase key
            return false

        copy value out
        return true
    */
    std::lock_guard<std::mutex> lock(mutex);

    auto it=setMap.find(key);
    if(it==setMap.end()) return {};

    if(it->second.expiry!=0 and it->second.expiry<Date_now()){
        setMap.erase(it);
        return {};
    }

    return it->second.value;
}

int StringStore::del(const std::vector<std::string>& keys){
    /*
        lock
        erase keys
        return number of keys deleted
    */
    int count=0;
    std::lock_guard<std::mutex> lock(mutex);
    
    for(const auto& key : keys){
        count+=(int)setMap.erase(key);
    }

    return count;
}
