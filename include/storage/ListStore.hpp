#pragma once

#include<string>
#include<vector>
#include<optional>
#include<mutex>
#include<condition_variable>
#include<unordered_map>

class ListStore{
public:
    int rpush(const std::string& key, const std::vector<std::string>& values);

    int lpush(const std::string& key, const std::vector<std::string>& values);

    int llen(const std::string& key);

    std::vector<std::string> lrange(const std::string& key, int start, int end);

    std::vector<std::string> lpop(const std::vector<std::string>& keys, int count);

    std::optional<std::pair<std::string, std::string>> blpop(const std::vector<std::string>& keys, int timeout);

    bool del(const std::string& key);

    bool exists(const std::string& key);
    
    std::vector<std::string> keys();

private:
    std::mutex mutex;
    std::condition_variable cv;
    std::unordered_map<std::string, std::vector<std::string>> lists;
};