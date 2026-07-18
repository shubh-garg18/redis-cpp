#pragma once 

#include<vector>
#include<string>
#include<functional>
#include<unordered_map>

#include "storage/Database.hpp"
#include "protocol/RESPMessage.hpp"

struct ClientState;
class PubSub;
struct AuthConfig;
class AofWriter;

struct Context{
    Database* db;
    std::string dir;
    std::string dbfilename;
    ClientState* client = nullptr;
    PubSub* pubsub = nullptr;
    AuthConfig* auth = nullptr;
    AofWriter* aof = nullptr;
};

using CommandHandler=
            std::function<
                std::string(
                    Context&, 
                    const std::vector<RESPMessage>&
                )
            >;

class Dispatcher{
private:
    std::unordered_map<std::string, CommandHandler> handlers;

public:
    void add(const std::string& cmd, CommandHandler fn);

    std::string executeCommand(Context& context,
                            const std::string& cmd,
                            const std::vector<RESPMessage>& args
    );    
};
std::string toUpper(const std::string& s);
