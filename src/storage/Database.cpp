#include "storage/Database.hpp"

#include <cstdint>
#include <cstdio>
#include <tuple>
#include <unordered_set>

Database::Database() {
    // Empty list: the two sentinels point at each other.
    lruHead.next = &lruTail;
    lruTail.prev = &lruHead;
}

Database::~Database() {
    LruNode* node = lruHead.next;
    while (node != &lruTail) {
        LruNode* next = node->next;
        delete node;
        node = next;
    }
}

std::string valueTypeName(ValueType type) {
    switch (type) {
        case ValueType::String: return "string";
        case ValueType::List: return "list";
        case ValueType::SortedSet: return "zset";
        case ValueType::Stream: return "stream";
        case ValueType::None: return "none";
    }
    return "none";
}

ValueType Database::typeOf(const std::string& key) {
    if (stringStore.exists(key)) return ValueType::String;
    if (listStore.exists(key)) return ValueType::List;
    if (sortedSetStore.exists(key)) return ValueType::SortedSet;
    if (streamStore.exists(key)) return ValueType::Stream;
    return ValueType::None;
}

bool Database::hasWrongType(const std::string& key, ValueType expected) {
    ValueType actual = typeOf(key);
    return actual != ValueType::None && actual != expected;
}

void Database::clearKey(const std::string& key) {
    del({key});
}

int Database::del(const std::vector<std::string>& keys) {
    int count = 0;
    for (const auto& key : keys) {
        bool removed = false;
        removed = stringStore.del({key}) > 0 || removed;
        removed = listStore.del(key) || removed;
        removed = sortedSetStore.del(key) || removed;
        removed = streamStore.del(key) || removed;
        if (removed) count++;
        forget(key);   // keep the LRU order in sync with the keyspace
    }
    return count;
}

std::vector<std::string> Database::keys(const std::string& pattern) {
    std::unordered_set<std::string> seen;
    std::vector<std::string> matched;

    auto add_matches = [&](const std::vector<std::string>& keys) {
        for (const auto& key : keys) {
            if ((pattern == "*" || key == pattern) && seen.insert(key).second) {
                matched.push_back(key);
            }
        }
    };

    add_matches(stringStore.keys());
    add_matches(listStore.keys());
    add_matches(sortedSetStore.keys());
    add_matches(streamStore.keys());
    return matched;
}

std::vector<std::vector<std::string>> Database::dumpAsCommands() {
    std::vector<std::vector<std::string>> cmds;

    for (const auto& [k, v, expiry] : stringStore.dump()) {
        if (expiry > 0) cmds.push_back({"SET", k, v, "PXAT", std::to_string(expiry)});
        else cmds.push_back({"SET", k, v});
    }

    for (const auto& k : listStore.keys()) {
        auto items = listStore.lrange(k, 0, -1);
        if (items.empty()) continue;
        std::vector<std::string> cmd{"RPUSH", k};
        for (const auto& item : items) cmd.push_back(item);
        cmds.push_back(std::move(cmd));
    }

    for (const auto& k : sortedSetStore.keys()) {
        auto members = sortedSetStore.zrange(k, 0, -1);
        std::vector<std::string> cmd{"ZADD", k};
        for (const auto& m : members) {
            auto score = sortedSetStore.zscore(k, m);
            if (!score) continue;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g", *score);
            cmd.push_back(buf);
            cmd.push_back(m);
        }
        if (cmd.size() > 2) cmds.push_back(std::move(cmd));
    }

    for (const auto& k : streamStore.keys()) {
        auto entries = streamStore.xrange(k, {0, 0}, {UINT64_MAX, UINT64_MAX});
        for (const auto& e : entries) {
            std::vector<std::string> cmd{"XADD", k, e.id.toString()};
            for (const auto& f : e.fields) cmd.push_back(f);
            cmds.push_back(std::move(cmd));
        }
    }

    return cmds;
}

uint64_t Database::version(const std::string& key) {
    std::lock_guard<std::mutex> lock(versionMutex);
    auto it = keyVersions.find(key);
    return it == keyVersions.end() ? 0 : it->second;
}

void Database::touch(const std::string& key) {
    std::lock_guard<std::mutex> lock(versionMutex);
    keyVersions[key]++;
}

void Database::setMaxKeys(size_t n) {
    maxKeys = n;
}

void Database::lruUnlink(LruNode* node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
}

void Database::lruPushFront(LruNode* node) {
    node->prev = &lruHead;
    node->next = lruHead.next;
    lruHead.next->prev = node;
    lruHead.next = node;
}

void Database::recordAccess(const std::string& key) {
    if (maxKeys == 0) return;                       // eviction disabled: don't track

    std::vector<std::string> evicted;
    {
        std::lock_guard<std::mutex> lock(lruMutex);

        // Re-check existence under the lock. Between the caller's read/write and
        // here another thread may have deleted the key; inserting a node for a
        // gone key would leave a phantom at the front that never ages out. If the
        // key is gone, drop any stale node we still hold and stop.
        if (typeOf(key) == ValueType::None) {
            auto gone = lruPos.find(key);
            if (gone != lruPos.end()) {
                lruUnlink(gone->second);
                delete gone->second;
                lruPos.erase(gone);
            }
            return;
        }

        auto it = lruPos.find(key);
        if (it != lruPos.end()) {
            lruUnlink(it->second);      // seen before: move it back to newest
            lruPushFront(it->second);
        } else {
            LruNode* node = new LruNode{key, nullptr, nullptr};
            lruPushFront(node);
            lruPos[key] = node;
        }

        // Over the cap: drop oldest keys (just before the tail sentinel).
        while (lruPos.size() > maxKeys) {
            LruNode* oldest = lruTail.prev;
            if (oldest == &lruHead) break;   // list already empty
            evicted.push_back(oldest->key);
            lruUnlink(oldest);
            lruPos.erase(oldest->key);
            delete oldest;
        }
    }

    // Delete the evicted keys from the stores outside lruMutex, so the lock
    // order is always lru -> store and never the reverse.
    for (const auto& k : evicted) {
        del({k});
        touch(k);   // bump version so a WATCH on an evicted key aborts
    }
}

void Database::forget(const std::string& key) {
    if (maxKeys == 0) return;
    std::lock_guard<std::mutex> lock(lruMutex);
    auto it = lruPos.find(key);
    if (it != lruPos.end()) {
        lruUnlink(it->second);
        delete it->second;
        lruPos.erase(it);
    }
}
