#include "command/AuthCommands.hpp"
#include "auth/AuthConfig.hpp"
#include "server/ClientState.hpp"
#include "protocol/RESPParser.hpp"

// AUTH [username] password. Only the implicit "default" user is supported, so a
// supplied username must be "default".
static std::string handleAuth(Context& ctx, const std::vector<RESPMessage>& args){
    if(args.size()!=1 && args.size()!=2)
        return encodeRESPError("ERR wrong number of arguments for 'auth' command");
    if(!ctx.auth || !ctx.client)
        return encodeRESPError("ERR auth not available");

    if(!ctx.auth->enabled())
        return encodeRESPError("ERR Client sent AUTH, but no password is set. Did you mean AUTH <username> <password>?");

    std::string username="default";
    std::string password;
    if(args.size()==2){
        username=args[0].str;
        password=args[1].str;
    } else {
        password=args[0].str;
    }

    // Plain compare: not constant-time, so technically a timing oracle, but
    // network jitter swamps it and real Redis only hardened this recently.
    if(username!="default" || password!=ctx.auth->requirepass)
        return encodeRESPError("WRONGPASS invalid username-password pair or user is disabled.");

    ctx.client->authenticated=true;
    return encodeRESPSimple("OK");
}

// ACL WHOAMI confirms who you are authed as; the rest of ACL is not modelled.
static std::string handleAcl(Context&, const std::vector<RESPMessage>& args){
    if(args.empty())
        return encodeRESPError("ERR wrong number of arguments for 'acl' command");
    if(toUpper(args[0].str)=="WHOAMI")
        return encodeRESPBulk("default");
    return encodeRESPError("ERR Unknown ACL subcommand '"+args[0].str+"'");
}

void registerAuthCommands(Dispatcher& dispatcher){
    dispatcher.add("AUTH", handleAuth);
    dispatcher.add("ACL", handleAcl);
}
