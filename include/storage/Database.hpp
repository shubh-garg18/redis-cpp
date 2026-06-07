#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "StringStore.hpp"
#include "ListStore.hpp"
#include "SortedSetStore.hpp"

enum class ValueType {
    None,
    String,
    List,
    SortedSet,
};

class Database {
public:
    StringStore stringStore;
    ListStore listStore;
    SortedSetStore sortedSetStore;

    int del(const std::vector<std::string>& keys);
    ValueType typeOf(const std::string& key);
    std::vector<std::string> keys(const std::string& pattern);

    bool hasWrongType(const std::string& key, ValueType expected);
    void clearKey(const std::string& key);

    uint64_t version(const std::string& key);
    void touch(const std::string& key);

private:
    std::mutex versionMutex;
    std::unordered_map<std::string, uint64_t> keyVersions;
};

std::string valueTypeName(ValueType type);
