#pragma once

#include "RESPMessage.hpp"
#include<string>
#include<vector>
#include <cstdint>

struct ParseResult{
    RESPMessage value;
    int len=0;
    bool ok=false;
    bool incomplete=false;
    std::string error;
};

ParseResult RESPParser(const std::string& chunk);

int64_t parse_int(const std::string& chunk, int start, int end);

std::string encodeRESPSimple(const std::string& s);
std::string encodeRESPError(const std::string& s);
std::string encodeRESPInteger(int64_t n);
std::string encodeRESPBulk(const std::string& s);
std::string encodeRESPArray(const std::vector<std::string>& items);
std::string encodeRESPArrayHeader(size_t size);
std::string encodeRESPNull();

// RESPParser.hpp