#include "persistence/AofWriter.hpp"

AofWriter::~AofWriter(){
    if(file) std::fclose(file);
}

bool AofWriter::open(const std::string& path){
    file=std::fopen(path.c_str(), "ab");
    return file!=nullptr;
}

void AofWriter::append(const std::string& resp){
    std::lock_guard<std::mutex> lock(mutex);
    if(!file) return;
    std::fwrite(resp.data(), 1, resp.size(), file);
    std::fflush(file);
}
