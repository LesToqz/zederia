#include "Transport.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <set>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace zmcp {

namespace {

struct StdioState {
    Server& server;
    std::FILE* out;
    std::mutex writeMutex;
    LegacyChannel legacy;
    std::mutex activeMutex;
    std::condition_variable activeChanged;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> active; // request id -> cancelled
    std::set<std::string> listening;                                    // open subscriptions/listen requests
    std::map<std::string, J> listenIds;
    int running = 0; // requests being handled
    int queued = 0;  // requests started and not yet finished (running or waiting)
    bool closing = false;

    void send(const J& message) {
        const std::string line = message.dump(-1, ' ', false, J::error_handler_t::replace) + "\n";
        std::lock_guard<std::mutex> lock(writeMutex);
        std::fwrite(line.data(), 1, line.size(), out);
        std::fflush(out);
    }
};

std::string idKey(const J& id) { return id.dump(); }

void runRequest(StdioState& s, J message, std::shared_ptr<std::atomic<bool>> cancelled) {
    {
        // At most four requests run at once; the rest wait their turn here, never in the reader,
        // so a cancellation still gets through (plan 4.8: limits).
        std::unique_lock<std::mutex> lock(s.activeMutex);
        s.activeChanged.wait(lock, [&] { return s.running < 4 || cancelled->load(); });
        ++s.running;
    }
    RequestInfo info;
    info.cancelled = [cancelled] { return cancelled->load(); };
    info.notify = [&s, cancelled](const J& n) {
        if (!cancelled->load())
            s.send(n);
    };
    J response;
    try {
        response = s.server.handle(message, &s.legacy, info);
    } catch (const Cancelled&) {
        response = nullptr;
    } catch (const std::exception& e) {
        response = Server::error(message.value("id", J()), kInternalError, std::string("Internal error: ") + e.what());
    }
    if (!response.is_null() && !cancelled->load())
        s.send(response);
    std::lock_guard<std::mutex> lock(s.activeMutex);
    s.active.erase(idKey(message["id"]));
    --s.running;
    --s.queued;
    s.activeChanged.notify_all();
}

void handleLine(StdioState& s, const std::string& line) {
    J m = J::parse(line, nullptr, false);
    if (m.is_discarded()) {
        s.send(Server::error(J(), kParseError, "Parse error: each line must be one JSON-RPC message"));
        return;
    }
    if (m.is_array()) {
        s.send(Server::error(J(), kInvalidRequest, "Invalid request: batches are not supported"));
        return;
    }
    if (!m.is_object()) {
        s.send(Server::error(J(), kInvalidRequest, "Invalid request: expected an object"));
        return;
    }
    if (m.contains("result") || m.contains("error"))
        return; // a response from the client: this server sends no requests, so there is nothing to match
    const std::string method = m.value("method", "");
    const bool isRequest = m.contains("id") && !m["id"].is_null();
    if (!isRequest) {
        if (method == "notifications/cancelled") {
            const J params = m.value("params", J::object());
            if (params.contains("requestId")) {
                const std::string key = idKey(params["requestId"]);
                std::lock_guard<std::mutex> lock(s.activeMutex);
                if (auto it = s.active.find(key); it != s.active.end())
                    it->second->store(true);
                s.listening.erase(key);
                s.listenIds.erase(key);
            }
            return;
        }
        RequestInfo info;
        s.server.handle(m, &s.legacy, info); // notifications/initialized and the like
        return;
    }
    if (method == "initialize") { // answered in order, before anything that follows it
        RequestInfo info;
        s.send(s.server.handle(m, &s.legacy, info));
        return;
    }
    if (method == "subscriptions/listen" && Server::eraOf(m) == Server::Era::Modern) {
        const J meta = m["params"]["_meta"];
        if (meta.value("io.modelcontextprotocol/protocolVersion", "") == kModernVersion) {
            // Nothing in this server's lists changes while it runs, so the stream acknowledges no
            // notification types and stays open until the client cancels it or the server ends.
            {
                std::lock_guard<std::mutex> lock(s.activeMutex);
                s.listening.insert(idKey(m["id"]));
                s.listenIds[idKey(m["id"])] = m["id"];
            }
            s.send({{"jsonrpc", "2.0"},
                    {"method", "notifications/subscriptions/acknowledged"},
                    {"params", {{"_meta", {{"io.modelcontextprotocol/subscriptionId", m["id"]}}}, {"notifications", J::object()}}}});
            return;
        }
    }
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    {
        std::unique_lock<std::mutex> lock(s.activeMutex);
        const std::string key = idKey(m["id"]);
        if (s.active.count(key)) {
            lock.unlock();
            s.send(Server::error(m["id"], kInvalidRequest, "Invalid request: this id is already in use by a request in progress"));
            return;
        }
        s.active[key] = cancelled;
        ++s.queued;
    }
    std::thread(runRequest, std::ref(s), std::move(m), cancelled).detach();
}

} // namespace

int runStdio(Server& server, std::FILE* in, std::FILE* out) {
#ifdef _WIN32
    _setmode(_fileno(in), _O_BINARY);
    _setmode(_fileno(out), _O_BINARY);
#endif
    StdioState s{server, out};
    std::string buffer;
    char chunk[1 << 15];
    bool tooLong = false;
    for (;;) {
        // A plain read returns what has arrived; fread would wait for a full buffer on a pipe.
#ifdef _WIN32
        const int got = _read(_fileno(in), chunk, static_cast<unsigned>(sizeof chunk));
#else
        const auto got = ::read(fileno(in), chunk, sizeof chunk);
#endif
        if (got <= 0)
            break;
        const auto n = static_cast<std::size_t>(got);
        buffer.append(chunk, n);
        std::size_t start = 0;
        for (std::size_t nl; (nl = buffer.find('\n', start)) != std::string::npos; start = nl + 1) {
            std::string line = buffer.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (tooLong) {
                tooLong = false;
                continue;
            }
            if (!line.empty())
                handleLine(s, line);
        }
        buffer.erase(0, start);
        if (buffer.size() > kMaxRequestBytes) {
            s.send(Server::error(J(), kInvalidRequest, "Invalid request: a message may be at most 16 MB"));
            buffer.clear();
            tooLong = true; // skip the rest of this line
        }
    }
    // Input closed: let what runs finish for a little while, then cancel it, end open
    // subscriptions and leave.
    std::unique_lock<std::mutex> lock(s.activeMutex);
    s.activeChanged.wait_for(lock, std::chrono::seconds(10), [&] { return s.queued == 0; });
    for (auto& [id, flag] : s.active)
        flag->store(true);
    s.activeChanged.notify_all();
    s.activeChanged.wait_for(lock, std::chrono::seconds(5), [&] { return s.queued == 0; });
    for (auto& [key, id] : s.listenIds)
        s.send({{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"resultType", "complete"}, {"_meta", {{"io.modelcontextprotocol/subscriptionId", id}}}}}});
    return 0;
}

} // namespace zmcp
