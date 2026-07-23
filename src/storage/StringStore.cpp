#include "storage/StringStore.hpp"

#include <stdexcept>

int64_t StringStore::Date_now(){
    using namespace std::chrono;
    auto now = steady_clock::now();
    auto ms =
        duration_cast<milliseconds>(
            now.time_since_epoch()
        ).count();
    return ms;
}

bool StringStore::isExpired(const Entry& entry, int64_t now) const {
    return entry.expiry != 0 && entry.expiry < now;
}

StringStore::Map::iterator StringStore::findLiveEntry(const std::string& key) {
    auto it = setMap.find(key);
    if (it == setMap.end()) return setMap.end();

    if (isExpired(it->second, Date_now())) {
        setMap.erase(it);
        return setMap.end();
    }

    return it;
}

int64_t StringStore::parseStoredInteger(const std::string& value) {
    size_t pos = 0;
    try {
        int64_t parsed = std::stoll(value, &pos);
        if (pos == value.size()) return parsed;
    } catch (...) {
    }
    throw std::runtime_error("ERR value is not an integer or out of range");
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

    auto it = findLiveEntry(key);
    if(it==setMap.end()) return {};

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

bool StringStore::exists(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex);
    return findLiveEntry(key) != setMap.end();
}

std::vector<std::string> StringStore::keys() {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::string> res;
    auto now = Date_now();
    for (auto it = setMap.begin(); it != setMap.end(); ) {
        if (isExpired(it->second, now)) {
            it = setMap.erase(it);
        } else {
            res.push_back(it->first);
            ++it;
        }
    }
    return res;
}

std::vector<std::tuple<std::string, std::string, int64_t>> StringStore::dump() {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::tuple<std::string, std::string, int64_t>> out;
    auto now = Date_now();
    for (auto it = setMap.begin(); it != setMap.end(); ) {
        if (isExpired(it->second, now)) {
            it = setMap.erase(it);
        } else {
            out.push_back({it->first, it->second.value, it->second.expiry});
            ++it;
        }
    }
    return out;
}

int64_t StringStore::incr(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex);

    auto it = findLiveEntry(key);
    int64_t expiry = 0;
    int64_t val = 1;

    if (it != setMap.end()) {
        expiry = it->second.expiry;
        val = parseStoredInteger(it->second.value) + 1;
    }

    Entry e;
    e.value = std::to_string(val);
    e.expiry = expiry;
    setMap[key] = e;
    return val;
}
