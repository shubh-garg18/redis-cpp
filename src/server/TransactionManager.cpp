#include "server/TransactionManager.hpp"

#include "protocol/RESPParser.hpp"

bool TransactionManager::shouldHandle(const std::string& cmd) const {
    std::string upper = toUpper(cmd);
    return (active or upper == "MULTI" or upper == "EXEC" or upper == "DISCARD" or upper == "WATCH" or upper == "UNWATCH");
}

std::string TransactionManager::handle(Context& context,
                                       Dispatcher& dispatcher,
                                       const std::string& cmd,
                                       std::vector<RESPMessage> args) {
    std::string upper = toUpper(cmd);

    if (upper == "WATCH") {
        if (active) return encodeRESPError("ERR WATCH inside MULTI is not allowed");
        if (args.empty()) return encodeRESPError("ERR wrong number of arguments for 'watch'");
        for (const auto& a : args) {
            watched[a.str] = context.db->version(a.str);
        }
        return encodeRESPSimple("OK");
    }

    if (upper == "MULTI") {
        if (active) return encodeRESPError("ERR MULTI calls can not be nested");
        active = true;
        return encodeRESPSimple("OK");
    }

    if (upper == "DISCARD") {
        if (!active) return encodeRESPError("ERR DISCARD without MULTI");
        active = false;
        queue.clear();
        watched.clear();
        return encodeRESPSimple("OK");
    }

    if (upper == "EXEC") {
        if (!active) return encodeRESPError("ERR EXEC without MULTI");

        bool dirty = false;
        for (const auto& [key, seen] : watched) {
            if (context.db->version(key) != seen) {
                dirty = true;
                break;
            }
        }

        active = false;
        watched.clear();

        if (dirty) {
            queue.clear();
            return "*-1\r\n";  // nil array: transaction aborted due to dirty watch
        }

        std::string reply = encodeRESPArrayHeader(queue.size());
        for (auto& queued : queue) {
            reply += dispatcher.executeCommand(context, queued.cmd, queued.args);
        }
        queue.clear();
        return reply;
    }

    if (upper == "UNWATCH") {
        watched.clear();
        return encodeRESPSimple("OK");
    }

    if (active) {
        queue.push_back({cmd, std::move(args)});
        return encodeRESPSimple("QUEUED");
    }

    return encodeRESPError("ERR command not handled by transaction manager");
}

        