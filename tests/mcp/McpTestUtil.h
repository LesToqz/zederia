// Helpers for the agent server's tests: an isolated set of folders per test process, requests in
// both protocol generations, and tool calls.
#pragma once

#include "src/Protocol.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <process.h>
#endif

namespace mcptest {

namespace fs = std::filesystem;
using zmcp::J;

inline void setEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

inline int pid() {
#ifdef _WIN32
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}

// A fresh folder tree for one test case: user, factory, settings and local folders. The settings
// and local folders are where the agent permissions and the link discovery files go.
inline fs::path freshRoot(const std::string& tag) {
    static std::atomic<int> counter{0};
    const fs::path root = fs::temp_directory_path() / ("zed_mcp_" + tag + "_" + std::to_string(pid()) + "_" + std::to_string(++counter));
    std::error_code ec;
    fs::remove_all(root, ec);
    for (const char* sub : {"user", "factory", "settings", "local", "outside"})
        fs::create_directories(root / sub);
    setEnv("ZEDERIA_SETTINGS_DIR", (root / "settings").string());
    setEnv("ZEDERIA_LOCAL_DIR", (root / "local").string());
    return root;
}

struct Rig {
    fs::path root;
    zmcp::Bridge bridge;
    zmcp::Server server;
    zmcp::LegacyChannel channel;
    int nextId = 1;

    static zmcp::Bridge::Config config(const fs::path& root, bool live) {
        zmcp::Bridge::Config c;
        c.userDir = (root / "user").string();
        c.factoryDir = (root / "factory").string();
        c.live = live;
        return c;
    }
    explicit Rig(const std::string& tag, bool live = false) : root(freshRoot(tag)), bridge(config(root, live)), server(bridge) {}

    static J meta(const std::string& version = zmcp::kModernVersion) {
        return {{"io.modelcontextprotocol/protocolVersion", version},
                {"io.modelcontextprotocol/clientCapabilities", J::object()},
                {"io.modelcontextprotocol/clientInfo", {{"name", "zederia-tests"}, {"title", "Tests"}, {"version", "1"}}}};
    }
    // A 2026-07-28 request.
    J modern(const std::string& method, J params = J::object()) {
        params["_meta"] = meta();
        return {{"jsonrpc", "2.0"}, {"id", nextId++}, {"method", method}, {"params", params}};
    }
    J send(const J& message, int* status = nullptr) {
        zmcp::RequestInfo info;
        return server.handle(message, &channel, info, status);
    }
    // tools/call in revision 2026-07-28; returns the result (content, structuredContent, isError).
    J call(const std::string& tool, const J& args) {
        const J r = send(modern("tools/call", {{"name", tool}, {"arguments", args}}));
        INFO(r.dump().substr(0, 2000));
        REQUIRE(r.contains("result"));
        return r["result"];
    }
    // A call that must succeed; returns its structured content.
    J ok(const std::string& tool, const J& args) {
        const J r = call(tool, args);
        INFO(tool << ": " << r.dump().substr(0, 3000));
        REQUIRE(r.value("isError", true) == false);
        REQUIRE(r.contains("structuredContent"));
        // The structured result validates against the tool's own output schema.
        const auto* def = server.findTool(tool);
        REQUIRE(def != nullptr);
        const auto problems = zmcp::validate(def->output, r["structuredContent"]);
        for (const auto& p : problems)
            UNSCOPED_INFO(tool << " output " << p.path << ": " << p.problem);
        CHECK(problems.empty());
        return r["structuredContent"];
    }
    // A call that must fail as a tool error; returns the error list.
    J error(const std::string& tool, const J& args) {
        const J r = call(tool, args);
        INFO(tool << ": " << r.dump().substr(0, 2000));
        REQUIRE(r.value("isError", false) == true);
        REQUIRE(r["content"].size() >= 2);
        const J details = J::parse(r["content"][1]["text"].get<std::string>(), nullptr, false);
        REQUIRE(details.contains("errors"));
        for (const auto& e : details["errors"]) {
            CHECK(e.contains("path"));
            CHECK_FALSE(e.value("problem", "").empty());
            CHECK_FALSE(e.value("fix", "").empty());
        }
        return details["errors"];
    }
};

inline bool anyPath(const J& errors, const std::string& path) {
    for (const auto& e : errors)
        if (e.value("path", "") == path)
            return true;
    return false;
}

inline bool anyText(const J& errors, const std::string& text) {
    for (const auto& e : errors)
        if (e.dump().find(text) != std::string::npos)
            return true;
    return false;
}

} // namespace mcptest
