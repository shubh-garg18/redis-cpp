#include "storage/Database.hpp"

#include <unordered_set>

std::string valueTypeName(ValueType type) {
    switch (type) {
        case ValueType::String: return "string";
        case ValueType::List: return "list";
        case ValueType::SortedSet: return "zset";
        case ValueType::None: return "none";
    }
    return "none";
}

ValueType Database::typeOf(const std::string& key) {
    if (stringStore.exists(key)) return ValueType::String;
    if (listStore.exists(key)) return ValueType::List;
    if (sortedSetStore.exists(key)) return ValueType::SortedSet;
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
        if (removed) count++;
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
    return matched;
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
