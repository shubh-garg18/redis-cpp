#include "command/CommandDispatcher.hpp"
#include "protocol/RESPParser.hpp"

#include<string>
#include<cctype>

std::string toUpper(const std::string& s){
    std::string cmd=s;
    for(char &c: cmd) c=(char)std::toupper((unsigned char)c);
    return cmd;
}

void Dispatcher::add(const std::string& cmd, CommandHandler fn){
    handlers[toUpper(cmd)]=fn;
}

std::string Dispatcher::executeCommand(Context& context,
                                       const std::string& cmd,
                                       const std::vector<RESPMessage>& args
){
    auto it=handlers.find(toUpper(cmd));
    if(it==handlers.end()) return encodeRESPError("ERR unknown command '" + cmd + "'");
    return it->second(context, args);
}