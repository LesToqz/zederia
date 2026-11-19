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

    const J status = rig.ok("get_status", J::object());
    REQUIRE(status["live_targets"].size() == 1);
    CHECK(status["live_targets"][0]["status"]["sample_rate"].get<double>() > 0.0);

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
    rig.ok("history", {{"target", target}, {"action", "revert_agent_session"}});
    CHECK(rig.ok("get_patch", {{"target", target}, {"form", "full"}})["patch"]["filter1"]["cutoff_hz"].get<double>() == Catch::Approx(original));
}
#endif
