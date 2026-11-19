// The transports (plan 4.3, 4.8, 6.1): stdio with concurrent requests and cancellation, and the
// loopback HTTP endpoint with its token, Origin and Host checks, the 2026-07-28 header rules,
// handshake sessions, event streams and cancellation by closing the stream.
#include "McpTestUtil.h"

#include "src/Transport.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <httplib.h>

using namespace mcptest;
using zmcp::J;

namespace {

std::vector<J> runStdioScript(zmcp::Server& server, const fs::path& dir, const std::vector<std::string>& lines) {
    const fs::path in = dir / "stdin.txt", out = dir / "stdout.txt";
    {
        std::ofstream f(in, std::ios::binary);
        for (const auto& l : lines)
            f << l << "\n";
    }
    std::FILE* fin = nullptr;
    std::FILE* fout = nullptr;
#ifdef _WIN32
    _wfopen_s(&fin, in.wstring().c_str(), L"rb");
    _wfopen_s(&fout, out.wstring().c_str(), L"wb");
#else
    fin = std::fopen(in.string().c_str(), "rb");
    fout = std::fopen(out.string().c_str(), "wb");
#endif
    REQUIRE(fin);
    REQUIRE(fout);
    zmcp::runStdio(server, fin, fout);
    std::fclose(fin);
    std::fclose(fout);
    std::ifstream f(out, std::ios::binary);
    std::vector<J> messages;
    std::string line;
    while (std::getline(f, line)) {
        J m = J::parse(line, nullptr, false);
        INFO(line);
        REQUIRE_FALSE(m.is_discarded()); // every line on stdout is one JSON-RPC message
        messages.push_back(m);
    }
    return messages;
}

J byId(const std::vector<J>& messages, const J& id) {
    for (const auto& m : messages)
        if (m.contains("id") && m["id"] == id && (m.contains("result") || m.contains("error")))
            return m;
    return nullptr;
}

std::string metaText() { return Rig::meta().dump(); }

std::string heavyNotes() {
    J notes = J::array();
    for (int i = 0; i < 8; ++i)
        notes.push_back({{"note", 48 + i * 3}, {"start", 0}, {"length", 110}});
    return notes.dump();
}

} // namespace

TEST_CASE("mcp: stdio carries both generations, rejects junk and honours cancellation", "[mcp][transport][stdio]") {
    Rig rig("stdio");
    const std::string m = metaText();
    const std::string heavy = R"({"osc1":{"unison":{"voices":16}},"osc2":{"on":true,"unison":{"voices":16}},"osc3":{"on":true,"unison":{"voices":16}}})";
    // A session to render in, opened first so its handle is known.
    zmcp::RequestInfo info;
    zmcp::LegacyChannel ch;
    const J opened = rig.server.handle(rig.modern("tools/call", {{"name", "open_session"}, {"arguments", J::object()}}), &ch, info);
    const std::string target = opened["result"]["structuredContent"]["target"];
    rig.ok("apply_patch", {{"target", target}, {"patch", J::parse(heavy)}});
    const std::vector<std::string> lines = {
        "this is not json",
        "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}]",
        R"({"jsonrpc":"2.0","id":"d","method":"server/discover","params":{"_meta":)" + m + "}}",
        R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{"_meta":)" + m + "}}",
        R"({"jsonrpc":"2.0","id":3,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t","version":"1"}}})",
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
        R"({"jsonrpc":"2.0","id":4,"method":"ping"})",
        R"({"jsonrpc":"2.0","id":"long","method":"tools/call","params":{"name":"render_audio","arguments":{"target":")" + target +
            R"(","notes":)" + heavyNotes() + R"(,"units":"seconds","name":"stdio_cancelled"},"_meta":{"progressToken":"p1"}}})",
        R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":"long","reason":"test"}})",
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"theory","arguments":{"action":"scale","key":"D dorian"}}})",
        R"({"jsonrpc":"2.0","id":6,"method":"subscriptions/listen","params":{"notifications":{"toolsListChanged":true},"_meta":)" + m + "}}",
    };
    const auto out = runStdioScript(rig.server, rig.root, lines);
    int parseErrors = 0, invalid = 0;
    for (const auto& msg : out) {
        if (msg.contains("error") && msg["id"].is_null()) {
            parseErrors += msg["error"]["code"] == -32700 ? 1 : 0;
            invalid += msg["error"]["code"] == -32600 ? 1 : 0;
        }
    }
    CHECK(parseErrors == 1);
    CHECK(invalid == 1);
    CHECK(byId(out, "d")["result"]["resultType"] == "complete");
    CHECK(byId(out, 2)["result"]["tools"].size() == 26);
    CHECK(byId(out, 3)["result"]["protocolVersion"] == "2025-06-18");
    CHECK(byId(out, 4)["result"] == J::object());
    CHECK(byId(out, 5)["result"]["structuredContent"]["scale"]["name"] == "D dorian");
    CHECK(byId(out, "long").is_null()); // cancelled: no answer
    bool acknowledged = false;
    for (const auto& msg : out)
        if (msg.value("method", "") == "notifications/subscriptions/acknowledged")
            acknowledged = msg["params"]["_meta"]["io.modelcontextprotocol/subscriptionId"] == 6 && msg["params"]["notifications"] == J::object();
    CHECK(acknowledged);
    CHECK(byId(out, 6)["result"]["_meta"]["io.modelcontextprotocol/subscriptionId"] == 6); // closed when input ended
    CHECK_FALSE(fs::exists(rig.root / "user" / "Renders" / "stdio_cancelled.wav"));
}

namespace {

struct HttpRig {
    std::string token = "test-token-0123456789abcdef"; // before the server, which is built with it
    Rig rig;
    zmcp::HttpServer http;
    int port = 0;

    explicit HttpRig(const std::string& tag) : rig(tag), http(rig.server, options()) {
        std::string error;
        INFO(error);
        REQUIRE(http.start(error));
        port = http.port();
    }
    zmcp::HttpOptions options() {
        zmcp::HttpOptions o;
        o.port = 0;
        o.token = token;
        return o;
    }
    httplib::Client client() {
        httplib::Client c("127.0.0.1", port);
        c.set_read_timeout(60, 0);
        return c;
    }
    httplib::Headers modernHeaders(const std::string& method, const std::string& name = {}) {
        httplib::Headers h = {{"Authorization", "Bearer " + token}, {"MCP-Protocol-Version", "2026-07-28"}, {"Mcp-Method", method},
                              {"Accept", "application/json, text/event-stream"}};
        if (!name.empty())
            h.emplace("Mcp-Name", name);
        return h;
    }
    static std::string body(const std::string& method, J params, int id = 1) {
        params["_meta"] = Rig::meta();
        return J{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}.dump();
    }
};

std::vector<J> events(const std::string& stream) {
    std::vector<J> out;
    std::istringstream in(stream);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("data: ", 0) == 0)
            out.push_back(J::parse(line.substr(6)));
    }
    return out;
}

} // namespace

TEST_CASE("mcp: HTTP refuses requests without the token, from other origins or hosts", "[mcp][transport][http][safety]") {
    HttpRig h("http_auth");
    auto c = h.client();
    const std::string body = HttpRig::body("tools/list", J::object());
    auto headers = h.modernHeaders("tools/list");
    auto noToken = headers;
    noToken.erase("Authorization");
    auto r = c.Post("/mcp", noToken, body, "application/json");
    REQUIRE(r);
    CHECK(r->status == 401);
    CHECK(r->get_header_value("WWW-Authenticate").find("Bearer") != std::string::npos);
    auto wrong = noToken;
    wrong.emplace("Authorization", "Bearer test-token-0123456789abcdeX");
    CHECK(c.Post("/mcp", wrong, body, "application/json")->status == 401);
    auto basic = noToken;
    basic.emplace("Authorization", "Basic dGVzdA==");
    CHECK(c.Post("/mcp", basic, body, "application/json")->status == 401);
    auto evil = headers;
    evil.emplace("Origin", "https://evil.example");
    r = c.Post("/mcp", evil, body, "application/json");
    CHECK(r->status == 403);
    CHECK(J::parse(r->body)["error"]["code"] == -32600);
    auto rebinding = headers;
    rebinding.emplace("Origin", "http://127.0.0.1.evil.example:" + std::to_string(h.port));
    CHECK(c.Post("/mcp", rebinding, body, "application/json")->status == 403);
    auto host = headers;
    host.emplace("Host", "attacker.example:" + std::to_string(h.port));
    CHECK(c.Post("/mcp", host, body, "application/json")->status == 403);
    auto local = headers;
    local.emplace("Origin", "http://localhost:3000");
    r = c.Post("/mcp", local, body, "application/json");
    CHECK(r->status == 200);
    CHECK(J::parse(r->body)["result"]["tools"].size() == 26);
    // GET offers no stream.
    CHECK(c.Get("/mcp", headers)->status == 405);
    CHECK(c.Get("/mcp", noToken)->status == 401);
    // The endpoint will not bind anywhere but loopback, and wants a real token.
    zmcp::HttpOptions o;
    o.host = "0.0.0.0";
    o.token = h.token;
    zmcp::HttpServer wide(h.rig.server, o);
    std::string error;
    CHECK_FALSE(wide.start(error));
    o.host = "127.0.0.1";
    o.token = "short";
    zmcp::HttpServer weak(h.rig.server, o);
    CHECK_FALSE(weak.start(error));
}

TEST_CASE("mcp: HTTP in revision 2026-07-28: headers must match the body", "[mcp][transport][http][modern]") {
    HttpRig h("http_modern");
    auto c = h.client();
    const std::string list = HttpRig::body("tools/list", J::object());
    auto r = c.Post("/mcp", h.modernHeaders("tools/list"), list, "application/json");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(J::parse(r->body)["result"]["resultType"] == "complete");
    // Missing or mismatched headers: 400 with HeaderMismatch.
    auto noVersion = h.modernHeaders("tools/list");
    noVersion.erase("MCP-Protocol-Version");
    r = c.Post("/mcp", noVersion, list, "application/json");
    CHECK(r->status == 400);
    CHECK(J::parse(r->body)["error"]["code"] == -32020);
    r = c.Post("/mcp", h.modernHeaders("prompts/list"), list, "application/json");
    CHECK(r->status == 400);
    CHECK(J::parse(r->body)["error"]["code"] == -32020);
    const std::string call = HttpRig::body("tools/call", {{"name", "get_status"}, {"arguments", J::object()}});
    r = c.Post("/mcp", h.modernHeaders("tools/call"), call, "application/json");
    CHECK(J::parse(r->body)["error"]["code"] == -32020); // Mcp-Name is required for tools/call
    r = c.Post("/mcp", h.modernHeaders("tools/call", "list_targets"), call, "application/json");
    CHECK(J::parse(r->body)["error"]["code"] == -32020);
    // The base64 form of Mcp-Name is decoded before comparing.
    r = c.Post("/mcp", h.modernHeaders("tools/call", "=?base64?Z2V0X3N0YXR1cw==?="), call, "application/json");
    REQUIRE(r->status == 200);
    CHECK(r->get_header_value("Content-Type").find("text/event-stream") != std::string::npos);
    auto ev = events(r->body);
    REQUIRE_FALSE(ev.empty());
    CHECK(ev.back()["result"]["structuredContent"]["server"]["name"] == "zederia-mcp");
    // Unsupported version and unknown method keep their status codes.
    J badVersion = J::parse(list);
    badVersion["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "1900-01-01";
    auto bv = h.modernHeaders("tools/list");
    bv.erase("MCP-Protocol-Version");
    bv.emplace("MCP-Protocol-Version", "1900-01-01");
    r = c.Post("/mcp", bv, badVersion.dump(), "application/json");
    CHECK(r->status == 400);
    CHECK(J::parse(r->body)["error"]["code"] == -32022);
    r = c.Post("/mcp", h.modernHeaders("tools/unknown"), HttpRig::body("tools/unknown", J::object()), "application/json");
    CHECK(r->status == 404);
    CHECK(J::parse(r->body)["error"]["code"] == -32601);
    // A tool call with a progress token streams its progress before the result.
    std::string target;
    for (const auto& e : events(c.Post("/mcp", h.modernHeaders("tools/call", "open_session"), HttpRig::body("tools/call", {{"name", "open_session"}, {"arguments", J::object()}}), "application/json")->body))
        if (e.contains("result"))
            target = e["result"]["structuredContent"]["target"];
    REQUIRE_FALSE(target.empty());
    J renderReq = J::parse(HttpRig::body("tools/call", {{"name", "render_audio"}, {"arguments", {{"target", target}, {"notes", {{{"note", 60}, {"start", 0}, {"length", 4}}}}, {"units", "seconds"}}}}));
    renderReq["params"]["_meta"]["progressToken"] = 7;
    r = c.Post("/mcp", h.modernHeaders("tools/call", "render_audio"), renderReq.dump(), "application/json");
    ev = events(r->body);
    int progress = 0;
    for (const auto& e : ev)
        progress += e.value("method", "") == "notifications/progress" && e["params"]["progressToken"] == 7 ? 1 : 0;
    CHECK(progress > 3);
    CHECK(ev.back()["result"]["isError"] == false);
    // A notification is accepted with 202 and no body.
    r = c.Post("/mcp", h.modernHeaders("notifications/cancelled"), R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":99}})", "application/json");
    CHECK(r->status == 202);
    CHECK(r->body.empty());
}

TEST_CASE("mcp: HTTP handshake sessions (2025-11-25)", "[mcp][transport][http][legacy]") {
    HttpRig h("http_legacy");
    auto c = h.client();
    httplib::Headers auth = {{"Authorization", "Bearer " + h.token}, {"Accept", "application/json, text/event-stream"}};
    const std::string list = R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})";
    auto r = c.Post("/mcp", auth, list, "application/json");
    CHECK(r->status == 400); // no session yet
    r = c.Post("/mcp", auth, R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"t","version":"1"}}})",
               "application/json");
    REQUIRE(r->status == 200);
    const std::string session = r->get_header_value("Mcp-Session-Id");
    REQUIRE(session.size() >= 16);
    CHECK(J::parse(r->body)["result"]["protocolVersion"] == "2025-11-25");
    auto withSession = auth;
    withSession.emplace("Mcp-Session-Id", session);
    withSession.emplace("MCP-Protocol-Version", "2025-11-25");
    CHECK(c.Post("/mcp", withSession, R"({"jsonrpc":"2.0","method":"notifications/initialized"})", "application/json")->status == 202);
    r = c.Post("/mcp", withSession, list, "application/json");
    REQUIRE(r->status == 200);
    CHECK(J::parse(r->body)["result"]["tools"].size() == 26);
    auto other = auth;
    other.emplace("Mcp-Session-Id", "0000000000000000000000000000000000");
    CHECK(c.Post("/mcp", other, list, "application/json")->status == 404);
    auto wrongVersion = auth;
    wrongVersion.emplace("Mcp-Session-Id", session);
    wrongVersion.emplace("MCP-Protocol-Version", "2025-06-18");
    CHECK(c.Post("/mcp", wrongVersion, list, "application/json")->status == 400);
    CHECK(c.Delete("/mcp", withSession)->status == 200);
    CHECK(c.Post("/mcp", withSession, list, "application/json")->status == 404); // ended
}

TEST_CASE("mcp: HTTP cancellation: closing the event stream stops the render", "[mcp][transport][http][cancel]") {
    HttpRig h("http_cancel");
    auto c = h.client();
    std::string target;
    for (const auto& e : events(c.Post("/mcp", h.modernHeaders("tools/call", "open_session"), HttpRig::body("tools/call", {{"name", "open_session"}, {"arguments", J::object()}}), "application/json")->body))
        if (e.contains("result"))
            target = e["result"]["structuredContent"]["target"];
    REQUIRE_FALSE(target.empty());
    h.rig.ok("apply_patch", {{"target", target},
                             {"patch", J::parse(R"({"osc1":{"unison":{"voices":16}},"osc2":{"on":true,"unison":{"voices":16}},"osc3":{"on":true,"unison":{"voices":16}}})")}});
    J req = J::parse(HttpRig::body("tools/call", {{"name", "render_audio"}, {"arguments", {{"target", target}, {"notes", J::parse(heavyNotes())}, {"units", "seconds"}, {"name", "http_cancelled"}}}}));
    req["params"]["_meta"]["progressToken"] = "c";
    httplib::Request hr;
    hr.method = "POST";
    hr.path = "/mcp";
    hr.headers = h.modernHeaders("tools/call", "render_audio");
    hr.set_header("Content-Type", "application/json");
    hr.body = req.dump();
    int chunks = 0;
    hr.content_receiver = [&](const char* data, std::size_t len, std::uint64_t, std::uint64_t) {
        if (std::string(data, len).find("notifications/progress") != std::string::npos)
            ++chunks;
        return chunks < 2; // close the stream after two progress notifications
    };
    const int before = h.rig.server.cancelledCount();
    c.send(hr);
    CHECK(chunks >= 1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (h.rig.server.cancelledCount() == before && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(h.rig.server.cancelledCount() == before + 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_FALSE(fs::exists(h.rig.root / "user" / "Renders" / "http_cancelled.wav"));
}
