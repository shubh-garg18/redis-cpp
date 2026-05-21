#pragma once

#include "RESPMessage.hpp"
#include<string>

struct ParseResult{
    RESPMessage value;
    int len=0;
    bool ok=false;
    bool incomplete=true;
    std::string error;
};

ParseResult RESPParser(const std::string& chunk);

int64_t parse_int(const std::string& chunk, int start, int end);

std::string encodeRESPSimple(const std::string& s);
std::string encodeRESPError(const std::string& s);
std::string encodeRESPInteger(long long n);
std::string encodeRESPBulk(const std::string& s);
std::string encodeRESPNull();

// RESPParser.hpp