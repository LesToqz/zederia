// zederia-mcp: the agent bridge (plan 4). Speaks the Model Context Protocol over stdio (default)
// or Streamable HTTP on the loopback interface (--http), to headless sessions in this process and
// to live Zederia instances through their local pipes. All protocol code lives in this program.
#include "src/Protocol.h"
#include "src/Transport.h"

#include "zederia/Version.h"
#include "zederia/link/Link.h"
#include "zederia/params/Registry.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

void usage() {
    std::fprintf(stderr,
                 "zederia-mcp %s - the Zederia agent bridge (Model Context Protocol 2026-07-28, 2025-11-25, 2025-06-18)\n\n"
                 "  zederia-mcp [--stdio]            serve one client over standard input and output (the default)\n"
                 "  zederia-mcp --http [--port N]    serve Streamable HTTP on 127.0.0.1 (default port 7425; 0 picks one)\n"
                 "        --token T                  the bearer token clients must send (default: a random one, written to\n"
                 "                                   %%LOCALAPPDATA%%\\Zederia\\mcp-http.json, readable by you only)\n"
                 "        --allow-origin O           also accept this Origin header (loopback origins are always accepted)\n"
                 "  --user-dir D       user content folder (default Documents\\Zederia)\n"
                 "  --factory-dir D    factory content folder\n"
                 "  --allow-read D     let agents read audio and images in D (repeatable)\n"
                 "  --allow-write D    let agents write in D (repeatable)\n"
                 "  --no-live          do not list or reach live instances\n"
                 "  --list-tools       print the tool definitions as JSON and exit\n"
                 "  --version          print the version and exit\n",
                 zed::kVersion);
}

} // namespace

int main(int argc, char** argv) {
    zmcp::Bridge::Config config;
    config.serverVersion = zed::kVersion;
    zmcp::HttpOptions http;
    bool useHttp = false, listTools = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "zederia-mcp: %s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--version") {
            std::printf("zederia-mcp %s (%d parameters; protocol 2026-07-28, 2025-11-25, 2025-06-18)\n", zed::kVersion, zed::Registry::get().size());
            return 0;
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else if (a == "--stdio") {
            useHttp = false;
        } else if (a == "--http") {
            useHttp = true;
        } else if (a == "--port") {
            http.port = std::atoi(next("--port").c_str());
        } else if (a == "--host") {
            http.host = next("--host");
        } else if (a == "--token") {
            http.token = next("--token");
        } else if (a == "--allow-origin") {
            http.allowOrigins.push_back(next("--allow-origin"));
        } else if (a == "--user-dir") {
            config.userDir = next("--user-dir");
        } else if (a == "--factory-dir") {
            config.factoryDir = next("--factory-dir");
        } else if (a == "--allow-read") {
            config.allowRead.push_back(next("--allow-read"));
        } else if (a == "--allow-write") {
            config.allowWrite.push_back(next("--allow-write"));
        } else if (a == "--no-live") {
            config.live = false;
        } else if (a == "--list-tools") {
            listTools = true;
        } else {
            std::fprintf(stderr, "zederia-mcp: unknown option %s\n\n", a.c_str());
            usage();
            return 2;
        }
    }

    zmcp::Bridge bridge(config);
    zmcp::Server server(bridge);
    if (listTools) {
        std::printf("%s\n", server.toolList().dump(2).c_str());
        return 0;
    }
    if (!useHttp) {
        std::fprintf(stderr, "zederia-mcp %s: serving MCP on standard input and output\n", zed::kVersion);
        const int code = zmcp::runStdio(server, stdin, stdout);
        std::fflush(stdout);
        std::_Exit(code); // request threads that outlived the grace period end with the process
    }
    if (http.token.empty())
        http.token = zed::link::randomHex(24);
    zmcp::HttpServer httpServer(server, http);
    std::string error;
    if (!httpServer.start(error)) {
        std::fprintf(stderr, "zederia-mcp: %s\n", error.c_str());
        return 1;
    }
    const std::string url = "http://127.0.0.1:" + std::to_string(httpServer.port()) + http.path;
    const std::string infoPath = zed::link::localDir() + "/mcp-http.json";
    std::string writeError;
    zed::link::writePrivateFile(infoPath, zmcp::J{{"url", url}, {"token", http.token}, {"pid", 0}}.dump(2), writeError);
    std::fprintf(stderr, "zederia-mcp %s: serving MCP on %s (bearer token in %s)\n", zed::kVersion, url.c_str(), infoPath.c_str());
    httpServer.wait();
    return 0;
}
