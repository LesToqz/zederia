// The live link against a running Standalone (plan 6.1): the real program is started with Agent
// Link on and its own folders, found through its discovery file, edited through the agent tools,
// and the edits read back and undone.
#include "McpTestUtil.h"

#include "zederia/link/Link.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace mcptest;
using zmcp::J;

#if defined(_WIN32) && defined(ZED_STANDALONE_EXE)
TEST_CASE("live link: a running Standalone takes agent edits and undoes them", "[mcp][live][standalone]") {
    Rig rig("standalone", true);
    // The child inherits this process's environment: its own settings and link folders, link on.
    setEnv("ZEDERIA_AGENT_LINK", "1");
    std::wstring cmd = L"\"" + zed::link::widen(ZED_STANDALONE_EXE) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;
    PROCESS_INFORMATION pi{};
    REQUIRE(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi));
    setEnv("ZEDERIA_AGENT_LINK", "");
    struct Kill {
        PROCESS_INFORMATION& pi;
        ~Kill() {
            TerminateProcess(pi.hProcess, 0);
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    } kill{pi};

    std::string target;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (target.empty() && std::chrono::steady_clock::now() < deadline) {
        const J listed = rig.ok("list_targets", J::object()); // keep the result alive while iterating it
        for (const auto& t : listed["targets"])
            if (t["kind"] == "live")
                target = t["target"];
        if (target.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (target.empty()) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        UNSCOPED_INFO("Standalone exit code " << code << " (259 = still running); link folder " << zed::link::discoveryDir());
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(zed::link::discoveryDir(), ec))
            UNSCOPED_INFO("  " << e.path().string());
    }
    REQUIRE_FALSE(target.empty());
    const auto d = zed::link::listDiscoveries();
    REQUIRE(d.size() == 1);
    CHECK(d[0].pid == static_cast<int>(pi.dwProcessId));
    CHECK(d[0].host == "Standalone");

    // The discovery file can appear before the program answers; wait until it does.
    J status;
    for (int i = 0; i < 120; ++i) {
        status = rig.ok("get_status", J::object());
        if (status["live_targets"].size() == 1 && status["live_targets"][0]["status"].contains("sample_rate"))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    REQUIRE(status["live_targets"].size() == 1);
    CHECK(status["live_targets"][0]["status"].value("sample_rate", 0.0) > 0.0);

    // The Standalone reopens the sound its last copy on this computer left (the framework keeps it in
    // the user's own application data, which the settings folder does not move), so start from Init.
    const J own = rig.ok("get_patch", {{"target", target}, {"form", "full"}})["patch"];
    rig.ok("apply_patch", {{"target", target}, {"patch", J::object()}, {"mode", "replace"}});
    const double original = rig.ok("get_patch", {{"target", target}, {"form", "full"}})["patch"]["filter1"]["cutoff_hz"];
    rig.ok("set_params", {{"target", target}, {"params", {{{"id", "filter1.on"}, {"value", true}}, {{"id", "filter1.cutoff"}, {"value", 777}}}}});
    CHECK(rig.ok("get_patch", {{"target", target}, {"form", "full"}})["patch"]["filter1"]["cutoff_hz"].get<double>() == Catch::Approx(777.0));
    const J h = rig.ok("history", {{"target", target}, {"action", "list"}});
    CHECK(h["steps"].back()["label"] == "Tests: Set parameters");
    CHECK(h["steps"].back()["origin"] == "Tests");
    rig.ok("history", {{"target", target}, {"action", "undo"}});
    CHECK(rig.ok("get_patch", {{"target", target}, {"form", "full"}})["patch"]["filter1"]["cutoff_hz"].get<double>() == Catch::Approx(original));
    // Renders of a live target happen in the server, on a copy of the instance's state.
    rig.ok("set_params", {{"target", target}, {"params", {{{"id", "filter1.cutoff"}, {"value", 1200}}}}});
    const J render = rig.ok("render_audio", {{"target", target}, {"notes", {{{"note", 57}, {"start", 0}, {"length", 0.5}}}}});
    CHECK(render["analysis"]["pitch"]["note"] == "A3");
    rig.ok("audition", {{"target", target}, {"notes", {{{"note", 57}, {"start", 0}, {"length", 0.2}, {"velocity", 30}}}}});
    // Reverting the agent's session brings back the instance's own sound, from before Init.
    rig.ok("history", {{"target", target}, {"action", "revert_agent_session"}});
    CHECK(rig.ok("get_patch", {{"target", target}, {"form", "full"}})["patch"] == own);
}
#endif

#if defined(_WIN32) && defined(ZED_MCP_EXE)
namespace {

// zederia-mcp on pipes, as a client runs it: stdin stays open and each request waits for its answer.
struct ServerProcess {
    PROCESS_INFORMATION pi{};
    HANDLE toChild = nullptr, fromChild = nullptr;
    std::string pending;

    explicit ServerProcess(const fs::path& root) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE childIn = nullptr, childOut = nullptr;
        REQUIRE(CreatePipe(&childIn, &toChild, &sa, 0));
        REQUIRE(CreatePipe(&fromChild, &childOut, &sa, 0));
        SetHandleInformation(toChild, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(fromChild, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = childIn;
        si.hStdOutput = childOut;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        std::wstring cmd = L"\"" + zed::link::widen(ZED_MCP_EXE) + L"\" --stdio --no-live --user-dir \"" + (root / "user").wstring() + L"\"";
        REQUIRE(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
        CloseHandle(childIn);
        CloseHandle(childOut);
    }
    ~ServerProcess() {
        if (toChild)
            CloseHandle(toChild); // end of input: the server should leave on its own
        const bool exited = WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0;
        if (!exited)
            TerminateProcess(pi.hProcess, 1);
        CloseHandle(fromChild);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    void send(const J& message) {
        const std::string line = message.dump() + "\n";
        DWORD written = 0;
        REQUIRE(WriteFile(toChild, line.data(), static_cast<DWORD>(line.size()), &written, nullptr));
    }
    // The next line the server writes, or null after `timeoutMs`.
    J receive(int timeoutMs = 10000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            if (const auto nl = pending.find('\n'); nl != std::string::npos) {
                J m = J::parse(pending.substr(0, nl), nullptr, false);
                pending.erase(0, nl + 1);
                return m;
            }
            DWORD available = 0;
            if (!PeekNamedPipe(fromChild, nullptr, 0, nullptr, &available, nullptr))
                return nullptr;
            if (available > 0) {
                std::string buf(available, '\0');
                DWORD got = 0;
                ReadFile(fromChild, buf.data(), available, &got, nullptr);
                pending.append(buf.data(), got);
                continue;
            }
            if (std::chrono::steady_clock::now() > deadline)
                return nullptr;
            Sleep(5);
        }
    }
};

} // namespace

TEST_CASE("mcp: the real program answers each request while its input stays open", "[mcp][transport][stdio][process]") {
    const fs::path root = freshRoot("process");
    {
        ServerProcess server(root);
        // The probe a dual-era client sends first, then the requests, one at a time.
        server.send({{"jsonrpc", "2.0"}, {"id", "probe"}, {"method", "server/discover"}, {"params", {{"_meta", Rig::meta()}}}});
        J r = server.receive();
        REQUIRE(r.is_object());
        CHECK(r["id"] == "probe");
        CHECK(r["result"]["supportedVersions"][0] == "2026-07-28");
        server.send({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}, {"params", {{"_meta", Rig::meta()}}}});
        r = server.receive();
        REQUIRE(r.is_object());
        CHECK(r["result"]["tools"].size() == 26);
    }
    {
        ServerProcess server(root);
        // A handshake client: initialize, initialized, then requests.
        server.send({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"},
                     {"params", {{"protocolVersion", "2025-06-18"}, {"capabilities", J::object()}, {"clientInfo", {{"name", "t"}, {"version", "1"}}}}}});
        J r = server.receive();
        REQUIRE(r.is_object());
        CHECK(r["result"]["protocolVersion"] == "2025-06-18");
        server.send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
        server.send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}, {"params", {{"_meta", {{"progressToken", 0}}}}}});
        r = server.receive();
        REQUIRE(r.is_object());
        CHECK(r["id"] == 1);
        CHECK(r["result"]["tools"].size() == 26);
    }
}
#endif
