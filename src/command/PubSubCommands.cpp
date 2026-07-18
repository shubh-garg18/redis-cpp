#include "command/PubSubCommands.hpp"
#include "server/ClientState.hpp"
#include "pubsub/PubSub.hpp"
#include "protocol/RESPParser.hpp"

// [kind, channel, running subscription count] — the confirmation Redis sends
// once per channel in a (UN)SUBSCRIBE call.
static std::string subReply(const std::string& kind, const std::string& channel, size_t count){
    std::string out=encodeRESPArrayHeader(3);
    out+=encodeRESPBulk(kind);
    out+=encodeRESPBulk(channel);
    out+=encodeRESPInteger((int64_t)count);
    return out;
}

static std::string handleSubscribe(Context& ctx, const std::vector<RESPMessage>& args){
    if(args.empty())
        return encodeRESPError("ERR wrong number of arguments for 'subscribe' command");

    std::string out;
    for(const auto& a : args){
        ctx.pubsub->subscribe(ctx.client, a.str);
        out+=subReply("subscribe", a.str, ctx.client->subscriptionCount());
    }
    return out;
}

static std::string handleUnsubscribe(Context& ctx, const std::vector<RESPMessage>& args){
    std::vector<std::string> channels;
    if(args.empty())
        channels.assign(ctx.client->channels.begin(), ctx.client->channels.end());
    else
        for(const auto& a : args) channels.push_back(a.str);

    if(channels.empty()){
        std::string out=encodeRESPArrayHeader(3);
        out+=encodeRESPBulk("unsubscribe");
        out+=encodeRESPNull();
        out+=encodeRESPInteger(0);
        return out;
    }

    std::string out;
    for(const auto& ch : channels){
        ctx.pubsub->unsubscribe(ctx.client, ch);
        out+=subReply("unsubscribe", ch, ctx.client->subscriptionCount());
    }
    return out;
}

static std::string handlePublish(Context& ctx, const std::vector<RESPMessage>& args){
    if(args.size()!=2)
        return encodeRESPError("ERR wrong number of arguments for 'publish' command");
    int delivered=ctx.pubsub->publish(args[0].str, args[1].str);
    return encodeRESPInteger(delivered);
}

static std::string handlePubsub(Context& ctx, const std::vector<RESPMessage>& args){
    if(args.empty())
        return encodeRESPError("ERR wrong number of arguments for 'pubsub' command");

    std::string sub=toUpper(args[0].str);
    if(sub=="CHANNELS")
        return encodeRESPArray(ctx.pubsub->listChannels());

    if(sub=="NUMSUB"){
        std::string out=encodeRESPArrayHeader((args.size()-1)*2);
        for(size_t i=1; i<args.size(); i++){
            out+=encodeRESPBulk(args[i].str);
            out+=encodeRESPInteger(ctx.pubsub->numSubscribers(args[i].str));
        }
        return out;
    }

    return encodeRESPError("ERR Unknown PUBSUB subcommand");
}

void registerPubSubCommands(Dispatcher& dispatcher){
    dispatcher.add("SUBSCRIBE", handleSubscribe);
    dispatcher.add("UNSUBSCRIBE", handleUnsubscribe);
    dispatcher.add("PUBLISH", handlePublish);
    dispatcher.add("PUBSUB", handlePubsub);
}
