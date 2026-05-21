#pragma once

#include<string>
#include<vector>
#include<cstdint>

struct RESPMessage{
    enum class Type{
        STR,
        ERR,
        INT,
        BULK,
        ARR,
        NIL,
    };
    Type type=Type::NIL;
    std::string str;
    int64_t n=0;
    std::vector<RESPMessage> arr;
};

// RESPMessage.hpp