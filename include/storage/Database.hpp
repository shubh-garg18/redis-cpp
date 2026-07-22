#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "StringStore.hpp"
#include "ListStore.hpp"
#include "SortedSetStore.hpp"
#include "StreamStore.hpp"

enum class ValueType {
    None,
    String,
    List,
    SortedSet,
    Stream,
};

// One link in the LRU order. The list runs newest -> oldest between two
// sentinel nodes, so every splice is a couple of pointer swaps, no edge cases.
struct LruNode {
    std::string key;
    LruNode* prev = nullptr;
    LruNode* next = nullptr;
};

class Database {
public:
    Database();
    ~Database();

    StringStore stringStore;
    ListStore listStore;
    SortedSetStore sortedSetStore;
    StreamStore streamStore;

    int del(const std::vector<std::string>& keys);
    ValueType typeOf(const std::string& key);
    std::vector<std::string> keys(const std::string& pattern);

    bool hasWrongType(const std::string& key, ValueType expected);
    void clearKey(const std::string& key);

    uint64_t version(const std::string& key);
    void touch(const std::string& key);

    // LRU eviction. Off unless setMaxKeys(n>0). recordAccess marks a key as
    // just-used (and evicts the oldest keys once the cap is passed); forget
    // drops a key that no longer exists so the order stays in sync.
    void setMaxKeys(size_t n);
    void recordAccess(const std::string& key);
    void forget(const std::string& key);

private:
    std::mutex versionMutex;
    std::unordered_map<std::string, uint64_t> keyVersions;

    std::mutex lruMutex;
    std::unordered_map<std::string, LruNode*> lruPos;
    LruNode lruHead;   // sentinel; lruHead.next is the newest key
    LruNode lruTail;   // sentinel; lruTail.prev is the oldest key
    size_t maxKeys = 0;

    void lruUnlink(LruNode* node);
    void lruPushFront(LruNode* node);
};

std::string valueTypeName(ValueType type);
