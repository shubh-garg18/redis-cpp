#pragma once

#include <string>
#include<unordered_map>
#include<vector>
#include<cstdint>
#include<mutex>
#include<optional>
#include<chrono>

class StringStore {
public:
    std::optional<std::string> get(const std::string& key);
    void set(const std::string& key, const std::string& value, int64_t expiry);
    int del(const std::vector<std::string>& keys); // Returns number of keys actually deleted.

    int64_t incr(const std::string& key);

    bool exists(const std::string& key);
    std::vector<std::string> keys();

    static int64_t Date_now();

private:
    struct Entry {
        std::string value;
        int64_t expiry;
    };
    using Map = std::unordered_map<std::string, Entry>;

    bool isExpired(const Entry& entry, int64_t now) const;
    Map::iterator findLiveEntry(const std::string& key);
    static int64_t parseStoredInteger(const std::string& value);

    std::mutex mutex;
    Map setMap;
};
