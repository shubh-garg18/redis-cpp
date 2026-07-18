#pragma once

#include <cstdio>
#include <mutex>
#include <string>

// Append-only file. Write commands are appended as RESP arrays as they run and
// replayed at startup. One instance, owned by main; append() is thread-safe.
class AofWriter {
public:
    ~AofWriter();

    bool open(const std::string& path);    // open for append; false on failure
    bool isOpen() const { return file != nullptr; }
    void append(const std::string& resp);  // thread-safe, flushes each write

private:
    std::FILE* file = nullptr;
    std::mutex mutex;
};
