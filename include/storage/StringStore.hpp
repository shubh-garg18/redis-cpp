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

    static int64_t Date_now();

private:
    struct Entry {
        std::string value;
        int64_t expiry;
    };
    std::mutex mutex;
    std::unordered_map<std::string, Entry> setMap;
};