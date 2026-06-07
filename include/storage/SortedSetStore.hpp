#pragma once

#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class SortedSetStore {
public:
    // Returns number of NEW members added (updates don't count).
    int zadd(const std::string& key,
             const std::vector<std::pair<std::string, double>>& entries);

    // 0-based rank by ascending score. nullopt if key or member missing.
    std::optional<int> zrank(const std::string& key, const std::string& member);

    // Members in rank range [start, end] inclusive. Negative indexes count from end.
    std::vector<std::string> zrange(const std::string& key, int start, int end);

    // 0 if key missing.
    int zcard(const std::string& key);

    // nullopt if key or member missing.
    std::optional<double> zscore(const std::string& key, const std::string& member);

    // Returns number of members actually removed.
    int zrem(const std::string& key, const std::vector<std::string>& members);

    // Returns true if the key existed.
    bool del(const std::string& key);

    bool exists(const std::string& key);
    std::vector<std::string> keys();

private:
    struct ZSet {
        std::unordered_map<std::string, double> scores;
        std::set<std::pair<double, std::string>> sorted;
    };
    std::mutex mutex;
    std::unordered_map<std::string, ZSet> sets;
};