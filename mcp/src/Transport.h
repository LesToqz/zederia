// The two transports (plan 4.3): stdio (the default; clients launch the server and talk over its
// standard streams) and Streamable HTTP bound to the loopback interface behind a flag, with
// Origin and Host validation and a bearer token (plan 4.8).
#pragma once

#include "Protocol.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace zmcp {

// Reads newline-delimited JSON-RPC from `in` and writes answers to `out` until `in` ends.
// Requests run on worker threads (at most four at a time), so a long render can be cancelled.
int runStdio(Server& server, std::FILE* in, std::FILE* out);

struct HttpOptions {
    std::string host = "127.0.0.1"; // loopback only: 127.0.0.1, ::1 or localhost
    int port = 7425;                // 0 picks a free port
    std::string token;              // required on every request as "Authorization: Bearer <token>"
    std::vector<std::string> allowOrigins; // extra allowed Origin values (exact)
    std::string path = "/mcp";
};

class HttpServer {
public:
    HttpServer(Server& server, HttpOptions options);
    ~HttpServer();
    bool start(std::string& error); // binds and starts listening on a thread
    void stop();
    int port() const { return port_; }
    void wait();                   // until stop() (or the process ends)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int port_ = 0;
};

} // namespace zmcp
