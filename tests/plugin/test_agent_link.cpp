// The live link (plan 4.2, 4.8, 6.1): a processor with Agent Link on, reached by the agent server
// through its pipe, while audio runs on its own thread under the audio-thread guard. Edits appear
// in the instance as undo steps labelled with the client's name and can be undone and reverted.
#include "../mcp/McpTestUtil.h"
#include "AgentLink.h"
#include "PluginProcessor.h"
#include "Session.h"

#include "zederia/link/Link.h"
#include "zederia/rt/AudioThreadGuard.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <future>
#include <thread>

using namespace mcptest;
using Catch::Approx;
using zmcp::J;

namespace {

// Runs `work` on a request thread while this (the message) thread services the instance, as the
// host's message loop would.
template <class F>
auto withMessageThread(ZederiaProcessor& p, F work) {
    auto future = std::async(std::launch::async, work);
    while (future.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready)
        p.serviceMessageThread();
    return future.get();
}

struct AudioThread {
    ZederiaProcessor& p;
    std::atomic<bool> running{true};
    std::atomic<int> blocks{0};
    std::thread thread;
    explicit AudioThread(ZederiaProcessor& processor) : p(processor) {
        thread = std::thread([this] {
            juce::AudioBuffer<float> buffer(2, 256);
            juce::MidiBuffer midi;
            midi.ensureSize(4096);
            while (running) {
                buffer.clear();
                p.processBlock(buffer, midi);
                midi.clear();
                ++blocks;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }
    ~AudioThread() {
        running = false;
        thread.join();
    }
};

} // namespace

TEST_CASE("live link: agent edits reach a playing instance, are labelled undo steps and can be reverted", "[plugin][live][agent]") {
    const fs::path root = freshRoot("live");
    ZederiaProcessor p;
    p.setPlayConfigDetails(0, 2, 48000.0, 256);
    p.prepareToPlay(48000.0, 256);
    const zed::Patch before = p.snapshot();
    std::string error;
    REQUIRE(p.agentLink().setEnabled(true, error));
    REQUIRE(p.agentLink().enabled());
    const auto discoveries = zed::link::listDiscoveries();
    REQUIRE(discoveries.size() == 1);
    CHECK(discoveries[0].instanceId == p.agentLink().discovery().instanceId);
    CHECK(discoveries[0].token.size() == 48);

    zmcp::Bridge::Config config;
    config.userDir = (root / "user").string();
    config.factoryDir = (root / "factory").string();
    zmcp::Bridge bridge(config);
    zmcp::Server server(bridge);
    zmcp::LegacyChannel channel;
    int id = 1;
    AudioThread* playing = nullptr; // set once audio runs: every call then lets two more blocks play
    const auto call = [&](const std::string& tool, const J& args) {
        J req = {{"jsonrpc", "2.0"}, {"id", id++}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", args}, {"_meta", Rig::meta()}}}};
        J result = withMessageThread(p, [&] {
            zmcp::RequestInfo info;
            return server.handle(req, &channel, info)["result"];
        });
        if (playing != nullptr) {
            const int target = playing->blocks.load() + 2;
            while (playing->blocks.load() < target)
                p.serviceMessageThread();
        }
        return result;
    };

    AudioThread audio(p); // the instance plays while the agent works
    playing = &audio;
    p.pushUiEvent(zed::MidiEvent::noteOn(0, 60, 0.8f));

    const J targets = call("list_targets", J::object())["structuredContent"]["targets"];
    REQUIRE(targets.size() == 1);
    const std::string live = targets[0]["target"];
    CHECK(live == "live:" + p.agentLink().discovery().instanceId);
    const J status = call("get_status", J::object())["structuredContent"];
    CHECK(status["live_targets"][0]["status"]["voices"].is_number());

    const int cutoff = zed::Registry::get().indexOf("filter1.cutoff");
    J r = call("set_params", {{"target", live}, {"params", {{{"id", "filter1.on"}, {"value", true}}, {{"id", "filter1.cutoff"}, {"value", "900 Hz"}}}}});
    REQUIRE(r["isError"] == false);
    CHECK(p.plainValue(cutoff) == Approx(900.0));
    const auto& h = p.session().history();
    CHECK(h.label(h.current()) == "Tests: Set parameters");
    CHECK(h.origin(h.current()) == "Tests");
    CHECK(call("get_patch", {{"target", live}, {"form", "full"}})["structuredContent"]["patch"]["filter1"]["cutoff_hz"].get<double>() == Approx(900.0));

    r = call("apply_patch", {{"target", live}, {"patch", {{"filter1", {{"cutoff_hz", 2500}}}}}, {"dry_run", true}});
    CHECK(r["structuredContent"]["applied"] == false);
    CHECK(p.plainValue(cutoff) == Approx(900.0));
    r = call("set_modulation", {{"target", live}, {"routes", {{{"source", "lfo1"}, {"dest", "filter1.cutoff"}, {"amount", 0.3}}}}});
    CHECK(p.routes().size() == 1);
    r = call("write_clip", {{"name", "riff"}, {"bars", 1}, {"tempo", 120}, {"key", "C minor"}, {"notes", {{{"start", 0}, {"length", 1}, {"key", 60}}, {{"start", 1}, {"length", 1}, {"key", 63}}}},
                            {"target", live}, {"slot", 2}, {"activate", true}});
    REQUIRE(r["isError"] == false);
    CHECK(p.clip(1).notes.size() == 2); // the clip player has the agent's clip
    CHECK(p.plainValue(zed::Registry::get().indexOf("clip.on")) == 1.0);

    // Undo through the agent, and by hand.
    r = call("history", {{"target", live}, {"action", "undo"}});
    REQUIRE(r["isError"] == false);
    CHECK(p.clip(1).notes.empty());
    CHECK(p.session().undo()); // the user's Ctrl+Z
    CHECK(p.routes().empty());
    CHECK(p.session().redo());

    // Audition through the instance, level-capped.
    r = call("audition", {{"target", live}, {"notes", {{{"note", 64}, {"start", 0}, {"length", 0.2}}}}});
    REQUIRE(r["isError"] == false);
    CHECK(p.auditioning());

    // The AGENT page's view.
    const auto clients = p.agentLink().clients();
    REQUIRE(clients.size() == 1); // the agent that asked, and nothing else
    CHECK(clients.front().name == "Tests");
    CHECK(clients.front().edits >= 4);
    bool logged = false;
    for (const auto& e : p.agentLink().activity())
        logged = logged || e.action == "Tests: Set parameters";
    CHECK(logged);

    // Revert agent session restores the sound from before the first agent edit.
    REQUIRE(p.agentLink().canRevert());
    r = call("history", {{"target", live}, {"action", "revert_agent_session"}});
    REQUIRE(r["isError"] == false);
    const zed::Patch reverted = p.snapshot();
    CHECK(reverted.get(cutoff) == Approx(before.get(cutoff)));
    CHECK(reverted.routes.empty());
    CHECK_FALSE(p.agentLink().canRevert());

    // Permissions from the AGENT page apply at once.
    zed::link::Permissions perms;
    perms.editPatch = false;
    REQUIRE(zed::link::savePermissions(perms, error));
    r = call("set_params", {{"target", live}, {"params", {{{"id", "filter1.cutoff"}, {"value", 300}}}}});
    CHECK(r["isError"] == true);
    CHECK(r["content"][0]["text"].get<std::string>().find("switched off") != std::string::npos);
    perms = zed::link::Permissions{};
    perms.audition = false;
    REQUIRE(zed::link::savePermissions(perms, error));
    CHECK(call("audition", {{"target", live}, {"notes", {{{"note", 64}, {"start", 0}, {"length", 0.2}}}}})["isError"] == true);
    REQUIRE(zed::link::savePermissions(zed::link::Permissions{}, error));

    CHECK(audio.blocks.load() > 20);
    p.pushUiEvent(zed::MidiEvent::noteOff(0, 60));

    // Switching the link off closes the pipe and removes the discovery file.
    p.agentLink().setEnabled(false, error);
    CHECK(zed::link::listDiscoveries().empty());
    CHECK(call("get_patch", {{"target", live}})["isError"] == true);
}

TEST_CASE("live link: the pipe wants the instance's token", "[plugin][live][agent][safety]") {
    freshRoot("token");
    ZederiaProcessor p;
    p.prepareToPlay(48000.0, 256);
    std::string error;
    REQUIRE(p.agentLink().setEnabled(true, error));
    const auto d = p.agentLink().discovery();
    const auto ask = [&](const J& request) {
        return withMessageThread(p, [&] {
            zed::link::PipeClient client; // no test assertions on this thread: they are not thread-safe
            std::string err, reply;
            if (!client.connect(d.pipe, 2000, err) || !client.request(request.dump(), reply, 5000, err))
                return J{{"ok", false}, {"error", {{"message", "transport: " + err}}}, {"transport", true}};
            return J::parse(reply);
        });
    };
    J r = ask({{"v", 1}, {"id", 1}, {"token", "wrong"}, {"method", "get_patch"}});
    CHECK(r["ok"] == false);
    CHECK(r["error"]["message"].get<std::string>().find("token") != std::string::npos);
    r = ask({{"v", 1}, {"id", 2}, {"method", "get_patch"}});
    CHECK(r["ok"] == false);
    r = ask({{"v", 2}, {"id", 3}, {"token", d.token}, {"method", "get_patch"}});
    CHECK(r["ok"] == false);
    r = ask({{"v", 1}, {"id", 4}, {"token", d.token}, {"method", "get_patch"}});
    CHECK(r["ok"] == true);
    CHECK_FALSE(r["result"]["patch"].get<std::string>().empty());
    r = ask({{"v", 1}, {"id", 5}, {"token", d.token}, {"method", "put_patch"}, {"params", {{"patch", "{not json"}}}});
    CHECK(r["ok"] == false);
    r = ask({{"v", 1}, {"id", 6}, {"token", d.token}, {"method", "put_patch"}, {"params", {{"patch", "{\"format\":\"zederia.patch\",\"version\":1}"}, {"expect_hash", "0"}}}});
    CHECK(r["error"]["message"].get<std::string>().find("changed meanwhile") != std::string::npos);
    r = ask({{"v", 1}, {"id", 7}, {"token", d.token}, {"method", "launch_missiles"}});
    CHECK(r["ok"] == false);
    // The link state travels with the host project.
    juce::MemoryBlock state;
    p.getStateInformation(state);
    p.agentLink().setEnabled(false, error);
    ZederiaProcessor q;
    q.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(q.agentLink().enabled());
    q.agentLink().setEnabled(false, error);
}
