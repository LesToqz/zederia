// Streamable HTTP on the loopback interface (plan 4.3, 4.8). Every request must come with the
// bearer token; an Origin header must be a loopback origin (or one allowed by flag), and the Host
// header must name the loopback address, so a web page cannot reach the server through DNS
// rebinding. Revision 2026-07-28 requests carry MCP-Protocol-Version, Mcp-Method and Mcp-Name
// headers that must match the body; handshake revisions use Mcp-Session-Id sessions.
#include "Transport.h"

#include "zederia/link/JsonLimits.h"
#include "zederia/link/Link.h"
#include "zederia/util/Compress.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include <httplib.h>

namespace zmcp {

namespace {

bool constantTimeEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool loopbackHost(const std::string& host) {
    const std::string h = lower(host);
    return h == "127.0.0.1" || h == "localhost" || h == "[::1]" || h == "::1";
}

// "http://127.0.0.1:7425" -> "127.0.0.1"; "" unless it is a well-formed origin: http or https,
// a host, an optional port of one to five digits, and nothing after it.
std::string originHost(const std::string& origin) {
    const auto scheme = origin.find("://");
    if (scheme == std::string::npos)
        return {};
    const std::string kind = lower(origin.substr(0, scheme));
    if (kind != "http" && kind != "https")
        return {};
    const std::string rest = origin.substr(scheme + 3);
    std::string host, port;
    if (!rest.empty() && rest.front() == '[') {
        const auto close = rest.find(']');
        if (close == std::string::npos)
            return {};
        host = rest.substr(0, close + 1);
        port = rest.substr(close + 1);
    } else {
        const auto colon = rest.find(':');
        host = rest.substr(0, colon);
        port = colon == std::string::npos ? std::string() : rest.substr(colon);
    }
    if (host.empty() || host.find_first_of("/@?#\\ ") != std::string::npos)
        return {};
    if (!port.empty()) {
        if (port.size() < 2 || port.size() > 6 || port.front() != ':')
            return {};
        for (std::size_t i = 1; i < port.size(); ++i)
            if (port[i] < '0' || port[i] > '9')
                return {};
    }
    return host;
}

std::string hostPart(const std::string& hostHeader) {
    if (!hostHeader.empty() && hostHeader.front() == '[')
        return hostHeader.substr(0, hostHeader.find(']') + 1);
    return hostHeader.substr(0, hostHeader.find(':'));
}

// "=?base64?...?=" header values (Mcp-Name) decode to UTF-8; plain values pass through.
bool decodeHeader(const std::string& v, std::string& out) {
    if (v.size() >= 11 && v.rfind("=?base64?", 0) == 0 && v.compare(v.size() - 2, 2, "?=") == 0) {
        std::vector<std::uint8_t> bytes;
        if (!zed::base64Decode(v.substr(9, v.size() - 11), bytes))
            return false;
        out.assign(bytes.begin(), bytes.end());
        return true;
    }
    for (const char c : v)
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t')
            return false;
    out = v;
    return true;
}

void jsonReply(httplib::Response& res, int status, const J& body) {
    res.status = status;
    if (!body.is_null())
        res.set_content(body.dump(), "application/json");
}

} // namespace

struct HttpServer::Impl {
    Server& server;
    HttpOptions options;
    // Open subscription streams. Declared before `http`, whose stream providers count it down
    // when they go, so it outlives them.
    std::atomic<int> listening{0};
    httplib::Server http;
    std::thread thread;
    std::mutex mutex;
    struct Session {
        std::shared_ptr<LegacyChannel> channel;
        std::map<std::string, std::shared_ptr<std::atomic<bool>>> active;
        std::chrono::steady_clock::time_point lastUsed;
    };
    std::map<std::string, Session> sessions;
    static constexpr std::size_t kMaxSessions = 64;
    static constexpr int kMaxListenStreams = 2; // of the endpoint's eight workers

    Impl(Server& s, HttpOptions o) : server(s), options(std::move(o)) {}

    bool originAllowed(const httplib::Request& req) {
        if (!req.has_header("Origin"))
            return true; // not a browser
        const std::string origin = req.get_header_value("Origin");
        for (const auto& o : options.allowOrigins)
            if (origin == o)
                return true;
        return loopbackHost(originHost(origin));
    }

    // Checks that apply to every request; false when an answer has been written.
    bool admit(const httplib::Request& req, httplib::Response& res) {
        if (!originAllowed(req)) {
            jsonReply(res, 403, Server::error(J(), kInvalidRequest, "Forbidden: the Origin is not allowed"));
            return false;
        }
        if (!loopbackHost(hostPart(req.get_header_value("Host")))) {
            jsonReply(res, 403, Server::error(J(), kInvalidRequest, "Forbidden: the Host header must name the loopback address"));
            return false;
        }
        const std::string auth = req.get_header_value("Authorization");
        const std::string prefix = "Bearer ";
        if (auth.size() <= prefix.size() || lower(auth.substr(0, prefix.size())) != lower(prefix) ||
            !constantTimeEqual(auth.substr(prefix.size()), options.token)) {
            res.set_header("WWW-Authenticate", "Bearer realm=\"zederia-mcp\"");
            jsonReply(res, 401, Server::error(J(), kInvalidRequest, "Unauthorized: send Authorization: Bearer <token> (the token zederia-mcp printed when it started)"));
            return false;
        }
        return true;
    }

    void post(const httplib::Request& req, httplib::Response& res) {
        if (!admit(req, res))
            return;
        if (req.body.size() > kMaxRequestBytes) {
            jsonReply(res, 413, Server::error(J(), kInvalidRequest, "Request too large"));
            return;
        }
        if (!zed::jsonNestingAllowed(req.body)) {
            jsonReply(res, 400, Server::error(J(), kInvalidRequest,
                                              "Invalid request: nested more than " + std::to_string(zed::kMaxJsonDepth) + " levels deep"));
            return;
        }
        J m = J::parse(req.body, nullptr, false);
        if (m.is_discarded()) {
            jsonReply(res, 400, Server::error(J(), kParseError, "Parse error"));
            return;
        }
        if (!m.is_object()) {
            jsonReply(res, 400, Server::error(J(), kInvalidRequest, m.is_array() ? "Invalid request: batches are not supported" : "Invalid request"));
            return;
        }
        const std::string method = argString(m, "method"); // not text: refused by Server::handle
        const bool isRequest = m.contains("id") && !m["id"].is_null();
        const J id = isRequest ? m["id"] : J();
        const Server::Era era = Server::eraOf(m);

        // Revision 2026-07-28 defines no notifications from the client over HTTP (closing a stream
        // cancels); one that arrives anyway is accepted and ignored.
        if (!isRequest && era != Server::Era::Legacy && req.get_header_value("MCP-Protocol-Version") == kModernVersion) {
            res.status = 202;
            return;
        }
        std::shared_ptr<LegacyChannel> channel;
        std::string sessionId;
        if (era == Server::Era::Modern) {
            const J& params = m["params"];
            const std::string version = argString(params["_meta"], "io.modelcontextprotocol/protocolVersion");
            const auto mismatch = [&](const std::string& what) {
                jsonReply(res, 400, Server::error(id, kHeaderMismatch, "Header mismatch: " + what));
            };
            if (!req.has_header("MCP-Protocol-Version"))
                return mismatch("the MCP-Protocol-Version header is missing");
            if (req.get_header_value("MCP-Protocol-Version") != version)
                return mismatch("MCP-Protocol-Version header value '" + req.get_header_value("MCP-Protocol-Version") + "' does not match body value '" + version + "'");
            if (!req.has_header("Mcp-Method"))
                return mismatch("the Mcp-Method header is missing");
            if (req.get_header_value("Mcp-Method") != method)
                return mismatch("Mcp-Method header value '" + req.get_header_value("Mcp-Method") + "' does not match body value '" + method + "'");
            if (method == "tools/call" || method == "prompts/get" || method == "resources/read") {
                const std::string body = method == "resources/read" ? argString(params, "uri") : argString(params, "name");
                std::string header;
                if (!req.has_header("Mcp-Name"))
                    return mismatch("the Mcp-Name header is missing");
                if (!decodeHeader(req.get_header_value("Mcp-Name"), header))
                    return mismatch("the Mcp-Name header is not a valid value");
                if (header != body)
                    return mismatch("Mcp-Name header value '" + header + "' does not match body value '" + body + "'");
            }
        } else if (method != "initialize") {
            // The handshake generation: a session from initialize.
            sessionId = req.get_header_value("Mcp-Session-Id");
            std::lock_guard<std::mutex> lock(mutex);
            auto it = sessions.find(sessionId);
            if (sessionId.empty()) {
                jsonReply(res, 400, Server::error(id, kInvalidRequest,
                                                  "Bad request: send initialize first (revision 2025-11-25 or 2025-06-18) and then the Mcp-Session-Id "
                                                  "it returns, or use revision 2026-07-28 with its _meta and headers",
                                                  {{"supported", supportedVersions()}}));
                return;
            }
            if (it == sessions.end()) {
                jsonReply(res, 404, Server::error(id, kInvalidRequest, "Session not found: start a new one with initialize"));
                return;
            }
            it->second.lastUsed = std::chrono::steady_clock::now();
            channel = it->second.channel;
            if (req.has_header("MCP-Protocol-Version") && req.get_header_value("MCP-Protocol-Version") != channel->version) {
                jsonReply(res, 400, Server::error(id, kInvalidRequest, "Bad request: MCP-Protocol-Version does not match the negotiated revision " + channel->version));
                return;
            }
        }

        if (!isRequest) {
            if (method == "notifications/cancelled" && !sessionId.empty()) {
                const J p = m.value("params", J::object());
                std::lock_guard<std::mutex> lock(mutex);
                auto it = sessions.find(sessionId);
                if (it != sessions.end() && p.contains("requestId"))
                    if (auto a = it->second.active.find(p["requestId"].dump()); a != it->second.active.end())
                        a->second->store(true);
            } else if (channel) {
                RequestInfo info;
                server.handle(m, channel.get(), info);
            }
            res.status = 202;
            return;
        }

        if (method == "initialize") {
            auto ch = std::make_shared<LegacyChannel>();
            RequestInfo info;
            J reply = server.handle(m, ch.get(), info);
            std::string newId;
            if (!zed::link::secureRandomHex(16, newId)) {
                jsonReply(res, 500, Server::error(id, kInternalError, "No secure random numbers for a session id"));
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                // Sessions unused for a day are forgotten, and at most kMaxSessions are kept: the
                // one unused longest makes room.
                const auto now = std::chrono::steady_clock::now();
                for (auto it = sessions.begin(); it != sessions.end();)
                    it = now - it->second.lastUsed > std::chrono::hours(24) ? sessions.erase(it) : std::next(it);
                while (sessions.size() >= kMaxSessions) {
                    auto oldest = sessions.begin();
                    for (auto it = sessions.begin(); it != sessions.end(); ++it)
                        if (it->second.lastUsed < oldest->second.lastUsed)
                            oldest = it;
                    sessions.erase(oldest);
                }
                sessions[newId] = Session{ch, {}, now};
            }
            res.set_header("Mcp-Session-Id", newId);
            jsonReply(res, 200, reply);
            return;
        }

        if (method == "subscriptions/listen" && era == Server::Era::Modern) {
            // Answered here, not by the server: check the revision as the server would.
            const std::string version = argString(m["params"]["_meta"], "io.modelcontextprotocol/protocolVersion");
            if (std::find(supportedVersions().begin(), supportedVersions().end(), version) == supportedVersions().end()) {
                jsonReply(res, 400, Server::error(id, kUnsupportedVersion, "Unsupported protocol version",
                                                  {{"supported", supportedVersions()}, {"requested", version}}));
                return;
            }
            // Each open stream holds one of the endpoint's few worker threads: at most a few at once.
            if (listening.fetch_add(1) >= kMaxListenStreams) {
                listening.fetch_sub(1);
                jsonReply(res, 429, Server::error(id, kInvalidRequest, "Too many open subscription streams: close one first"));
                return;
            }
            // No list changes while the server runs: acknowledge nothing and keep the stream
            // open (with keep-alive comments) until the client closes it.
            res.set_header("X-Accel-Buffering", "no");
            res.set_header("Cache-Control", "no-cache");
            auto counted = std::shared_ptr<std::atomic<int>>(&listening, [](std::atomic<int>* n) { n->fetch_sub(1); });
            res.set_chunked_content_provider("text/event-stream", [id, counted](std::size_t, httplib::DataSink& sink) {
                const J ack = {{"jsonrpc", "2.0"},
                               {"method", "notifications/subscriptions/acknowledged"},
                               {"params", {{"_meta", {{"io.modelcontextprotocol/subscriptionId", id}}}, {"notifications", J::object()}}}};
                std::string ev = "event: message\ndata: " + ack.dump() + "\n\n";
                if (!sink.write(ev.data(), ev.size()))
                    return false;
                for (int i = 0; i < 2 * 3600 * 4; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    if (i % 60 == 59 && !sink.write(":\n\n", 3))
                        return false;
                    if (!sink.is_writable())
                        return false;
                }
                sink.done();
                return true;
            });
            return;
        }

        // A tools/call on the modern revision, or any request that asked for progress, answers on an
        // event stream: progress arrives on it, and closing it cancels the request.
        const bool wantsStream = (era == Server::Era::Modern && method == "tools/call") ||
                                 (m.contains("params") && m["params"].is_object() && m["params"].contains("_meta") &&
                                  m["params"]["_meta"].is_object() && m["params"]["_meta"].contains("progressToken"));
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        if (!sessionId.empty()) {
            // find, never operator[]: a DELETE since the lookup above must not bring the session
            // back without its channel.
            std::lock_guard<std::mutex> lock(mutex);
            auto it = sessions.find(sessionId);
            if (it == sessions.end()) {
                jsonReply(res, 404, Server::error(id, kInvalidRequest, "Session not found: start a new one with initialize"));
                return;
            }
            // Ids of requests in flight must differ (JSON-RPC): one cancellation entry each.
            if (!it->second.active.emplace(id.dump(), cancelled).second) {
                jsonReply(res, 400, Server::error(id, kInvalidRequest, "Invalid request: a request with this id is still running"));
                return;
            }
        }
        const auto forget = [this, sessionId, id] {
            if (sessionId.empty())
                return;
            std::lock_guard<std::mutex> lock(mutex);
            if (auto it = sessions.find(sessionId); it != sessions.end())
                it->second.active.erase(id.dump());
        };
        if (!wantsStream) {
            RequestInfo info;
            info.cancelled = [cancelled] { return cancelled->load(); };
            int status = 200;
            J reply;
            try {
                reply = server.handle(m, channel.get(), info, &status);
            } catch (const Cancelled&) {
            }
            forget();
            if (reply.is_null()) {
                res.status = 204;
                return;
            }
            jsonReply(res, status, reply);
            return;
        }
        // Errors decided before any work (unsupported version, missing capabilities) keep their status.
        {
            RequestInfo probe;
            J metaOnly = m;
            metaOnly["method"] = "ping-check";
            int status = 200;
            const J early = server.handle(metaOnly, channel.get(), probe, &status);
            if (early.contains("error") && early["error"].value("code", 0) != kMethodNotFound) {
                forget();
                early.is_null() ? void() : jsonReply(res, status, Server::error(id, early["error"]["code"], early["error"]["message"],
                                                                            early["error"].value("data", J())));
                return;
            }
        }
        res.set_header("X-Accel-Buffering", "no");
        res.set_header("Cache-Control", "no-cache");
        Server* srv = &server;
        res.set_chunked_content_provider("text/event-stream", [srv, m, channel, cancelled, forget](std::size_t, httplib::DataSink& sink) {
            std::mutex writeMutex;
            bool broken = false;
            auto lastBeat = std::chrono::steady_clock::now();
            const auto write = [&](const std::string& text) {
                std::lock_guard<std::mutex> lock(writeMutex);
                if (broken)
                    return false;
                if (!sink.write(text.data(), text.size()))
                    broken = true;
                return !broken;
            };
            RequestInfo info;
            info.notify = [&](const J& n) { write("event: message\ndata: " + n.dump() + "\n\n"); };
            info.cancelled = [&]() {
                if (cancelled->load())
                    return true;
                const auto now = std::chrono::steady_clock::now();
                if (now - lastBeat > std::chrono::milliseconds(250)) { // a closed stream is the cancellation
                    lastBeat = now;
                    if (!write(":\n\n") || !sink.is_writable())
                        cancelled->store(true);
                }
                return cancelled->load();
            };
            J reply;
            try {
                reply = srv->handle(m, channel.get(), info);
            } catch (const Cancelled&) {
            } catch (const std::exception& e) {
                reply = Server::error(m["id"], kInternalError, std::string("Internal error: ") + e.what());
            }
            forget();
            if (!reply.is_null() && !cancelled->load())
                write("event: message\ndata: " + reply.dump() + "\n\n");
            sink.done();
            return true;
        });
    }
};

HttpServer::HttpServer(Server& server, HttpOptions options) : impl_(std::make_unique<Impl>(server, std::move(options))) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(std::string& error) {
    auto& o = impl_->options;
    if (!loopbackHost(o.host)) {
        error = "the HTTP endpoint binds to the loopback interface only (127.0.0.1, ::1 or localhost)";
        return false;
    }
    if (o.token.size() < 16) {
        error = "the bearer token must have at least 16 characters";
        return false;
    }
    auto& http = impl_->http;
    http.new_task_queue = [] { return new httplib::ThreadPool(8); };
    http.set_payload_max_length(kMaxRequestBytes + 1024);
    http.set_read_timeout(30, 0);
    http.set_write_timeout(30, 0);
    http.set_keep_alive_max_count(100);
    http.Post(o.path, [this](const httplib::Request& req, httplib::Response& res) {
        // A message the checks let through and that still throws is the client's fault, not a 500.
        try {
            impl_->post(req, res);
        } catch (const std::exception& e) {
            jsonReply(res, 400, Server::error(J(), kInvalidRequest, std::string("Invalid request: ") + e.what()));
        }
    });
    http.Get(o.path, [this](const httplib::Request& req, httplib::Response& res) {
        if (!impl_->admit(req, res))
            return;
        res.set_header("Allow", "POST");
        jsonReply(res, 405, Server::error(J(), kInvalidRequest, "Method not allowed: this server offers no GET stream; POST requests"));
    });
    http.Delete(o.path, [this](const httplib::Request& req, httplib::Response& res) {
        if (!impl_->admit(req, res))
            return;
        const std::string sid = req.get_header_value("Mcp-Session-Id");
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (sid.empty()) {
            res.set_header("Allow", "POST");
            res.status = 405;
            return;
        }
        res.status = impl_->sessions.erase(sid) ? 200 : 404;
    });
    const std::string bindHost = lower(o.host) == "localhost" ? "127.0.0.1" : (o.host == "[::1]" ? "::1" : o.host);
    if (o.port == 0) {
        port_ = http.bind_to_any_port(bindHost);
        if (port_ <= 0) {
            error = "cannot bind to " + bindHost;
            return false;
        }
    } else {
        if (!http.bind_to_port(bindHost, o.port)) {
            error = "cannot bind to " + bindHost + ":" + std::to_string(o.port) + " (is the port in use?)";
            return false;
        }
        port_ = o.port;
    }
    impl_->thread = std::thread([this] { impl_->http.listen_after_bind(); });
    return true;
}

void HttpServer::stop() {
    if (impl_->thread.joinable()) {
        impl_->http.stop();
        impl_->thread.join();
    }
}

void HttpServer::wait() {
    if (impl_->thread.joinable())
        impl_->thread.join();
}

} // namespace zmcp
