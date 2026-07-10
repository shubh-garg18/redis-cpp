#pragma once

#include<string>
#include<vector>
#include<utility>
#include<cstdint>
#include<mutex>
#include<condition_variable>
#include<unordered_map>

struct StreamID {
    uint64_t ms=0;
    uint64_t seq=0;

    bool operator==(const StreamID& o) const { return ms==o.ms and seq==o.seq; }
    bool operator<(const StreamID& o) const { return ms!=o.ms ? ms<o.ms : seq<o.seq; }
    bool operator<=(const StreamID& o) const { return !(o<*this); }

    std::string toString() const { return std::to_string(ms)+"-"+std::to_string(seq); }
};

struct StreamEntry {
    StreamID id;
    std::vector<std::string> fields; // flat [field, value, field, value, ...]
};

struct XAddResult {
    bool ok=false;
    StreamID id;
    std::string error;
};

class StreamStore {
public:
    // ms/seq are -1 when XADD must generate them: "*" -> -1,-1   "5-*" -> 5,-1
    XAddResult xadd(const std::string& key, int64_t ms, int64_t seq,
                    const std::vector<std::string>& fields);

    // Entries with start <= id <= end, both ends inclusive.
    std::vector<StreamEntry> xrange(const std::string& key, StreamID start, StreamID end);

    // Per (key, after) query, entries with id strictly greater than after.
    // Keys with nothing new are dropped from the result.
    // block_ms < 0: return now.  == 0: block forever.  > 0: block at most that long.
    std::vector<std::pair<std::string, std::vector<StreamEntry>>>
    xread(const std::vector<std::pair<std::string, StreamID>>& queries, int64_t block_ms);

    // Highest id stored, for resolving "$" in XREAD. {0,0} if key missing.
    StreamID lastId(const std::string& key);

    bool del(const std::string& key);

    bool exists(const std::string& key);

    std::vector<std::string> keys();

private:
    struct Stream {
        std::vector<StreamEntry> entries; // append-only, ascending by id
        StreamID top;
    };

    // caller must already hold mutex
    std::vector<std::pair<std::string, std::vector<StreamEntry>>>
    collect(const std::vector<std::pair<std::string, StreamID>>& queries);

    std::mutex mutex;
    std::condition_variable cv;
    std::unordered_map<std::string, Stream> streams;
};
