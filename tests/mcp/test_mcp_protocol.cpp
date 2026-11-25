// Protocol conformance (plan 6.1): scripted sessions in each generation, every tool's inputs and
// outputs against its schemas, error paths, and cancellation of long renders.
#include "McpTestUtil.h"

#include "zederia/link/JsonLimits.h"
#include "zederia/link/Link.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include <fstream>
#include <regex>
#include <map>
#include <set>

using namespace mcptest;
using zmcp::J;

TEST_CASE("mcp: the catalogue: order, names, annotations and schemas the validator understands", "[mcp][schema]") {
    Rig rig("catalogue");
    const std::vector<std::string> plan = {"get_status", "list_targets", "open_session", "close_session", "describe_synth", "get_patch",
                                           "apply_patch", "set_params", "set_modulation", "set_shape", "history", "create_wavetable",
                                           "edit_wavetable", "list_wavetables", "list_presets", "load_preset", "save_preset", "tag_presets",
                                           "render_audio", "analyze_audio", "compare_audio", "audition", "theory", "write_clip",
                                           "get_clip", "export_pack"};
    const J list = rig.server.toolList();
    REQUIRE(list.size() == plan.size());
    const std::regex name("^[A-Za-z0-9_.-]{1,128}$");
    for (std::size_t i = 0; i < plan.size(); ++i) {
        const J& t = list[i];
        INFO(t["name"]);
        CHECK(t["name"] == plan[i]);
        CHECK(std::regex_match(t["name"].get<std::string>(), name));
        CHECK(t["inputSchema"]["type"] == "object");
        CHECK(t.contains("outputSchema"));
        CHECK(t["annotations"]["readOnlyHint"].is_boolean());
        CHECK(t["annotations"]["destructiveHint"].is_boolean());
        CHECK(t["annotations"]["idempotentHint"].is_boolean());
        CHECK(t["annotations"]["openWorldHint"] == false);
        CHECK(t["description"].get<std::string>().size() > 40);
        CHECK(zmcp::unknownKeywords(t["inputSchema"]).empty());
        CHECK(zmcp::unknownKeywords(t["outputSchema"]).empty());
        // An empty object is valid input exactly when nothing is required.
        const bool required = t["inputSchema"].contains("required");
        CHECK(zmcp::validate(t["inputSchema"], J::object()).empty() == !required);
    }
    CHECK(rig.server.toolList() == list); // the same order every time
    // Edits of a target are undo steps, so they are not destructive; what cannot be undone is.
    std::map<std::string, bool> destructive;
    for (const auto& t : list)
        destructive[t["name"]] = t["annotations"]["destructiveHint"];
    for (const char* undoable : {"apply_patch", "set_params", "set_modulation", "set_shape", "history", "load_preset"})
        CHECK_FALSE(destructive[undoable]);
    for (const char* lasting : {"close_session", "tag_presets", "edit_wavetable"})
        CHECK(destructive[lasting]);
    for (const char* reading : {"get_status", "list_targets", "describe_synth", "get_patch", "list_presets", "list_wavetables", "analyze_audio", "compare_audio", "theory", "get_clip"})
        CHECK(list[std::distance(plan.begin(), std::find(plan.begin(), plan.end(), reading))]["annotations"]["readOnlyHint"] == true);
}

TEST_CASE("mcp: the validator reports paths and fixes", "[mcp][schema]") {
    const J s = J::parse(R"({"type":"object","additionalProperties":false,"required":["mode"],"properties":{
        "mode":{"enum":["merge","replace"]},"n":{"type":"integer","minimum":1,"maximum":8},
        "list":{"type":"array","maxItems":2,"items":{"type":"number"}},"either":{"oneOf":[{"type":"string"},{"type":"array"}]}}})");
    CHECK(zmcp::validate(s, J{{"mode", "merge"}, {"n", 3}}).empty());
    auto p = zmcp::validate(s, J{{"mode", "mrege"}, {"n", 9}, {"lst", 1}, {"list", {1, "x", 3}}, {"either", 2}});
    std::set<std::string> paths;
    for (const auto& x : p)
        paths.insert(x.path);
    CHECK(paths.count("/mode"));
    CHECK(paths.count("/n"));
    CHECK(paths.count("/lst"));
    CHECK(paths.count("/list"));
    CHECK(paths.count("/list/1"));
    CHECK(paths.count("/either"));
    for (const auto& x : p) {
        if (x.path == "/mode")
            CHECK(x.fix.find("did you mean \"merge\"") != std::string::npos);
        if (x.path == "/lst")
            CHECK(x.fix.find("did you mean \"list\"") != std::string::npos);
    }
    CHECK(zmcp::validate(s, J::object()).front().path == "/mode");
}

TEST_CASE("mcp: a revision 2026-07-28 session", "[mcp][protocol][modern]") {
    Rig rig("modern");
    const J d = rig.send(rig.modern("server/discover"));
    REQUIRE(d.contains("result"));
    CHECK(d["result"]["resultType"] == "complete");
    CHECK(d["result"]["supportedVersions"] == J({"2026-07-28", "2025-11-25", "2025-06-18"}));
    CHECK(d["result"]["capabilities"].contains("tools"));
    CHECK(d["result"]["ttlMs"].get<int>() >= 0);
    CHECK(d["result"]["cacheScope"] == "public");
    CHECK(d["result"]["_meta"]["io.modelcontextprotocol/serverInfo"]["name"] == "zederia-mcp");
    for (const char* method : {"tools/list", "resources/list", "resources/templates/list", "prompts/list"}) {
        INFO(method);
        const J r = rig.send(rig.modern(method));
        REQUIRE(r.contains("result"));
        CHECK(r["result"]["resultType"] == "complete");
        CHECK(r["result"]["ttlMs"].is_number_integer());
        CHECK(r["result"]["cacheScope"] == "public");
    }
    for (const char* uri : {"zederia://guide/agent", "zederia://schema/patch", "zederia://schema/params"}) {
        INFO(uri);
        const J r = rig.send(rig.modern("resources/read", {{"uri", uri}}));
        REQUIRE(r.contains("result"));
        CHECK(r["result"]["resultType"] == "complete");
        CHECK(r["result"]["cacheScope"].is_string());
        const std::string text = r["result"]["contents"][0]["text"].get<std::string>();
        CHECK(text.size() > 200);
        if (std::string(uri).rfind("zederia://schema", 0) == 0)
            CHECK_FALSE(J::parse(text, nullptr, false).is_discarded());
    }
    int status = 0;
    const J missing = rig.send(rig.modern("resources/read", {{"uri", "zederia://renders/none"}}), &status);
    CHECK(missing["error"]["code"] == -32602);
    const J prompt = rig.send(rig.modern("prompts/get", {{"name", "design_sound"}, {"arguments", {{"brief", "a warm pluck"}}}}));
    CHECK(prompt["result"]["messages"][0]["content"]["text"].get<std::string>().find("a warm pluck") != std::string::npos);
    for (const char* p : {"match_reference", "write_progression_loop", "write_melody_loop", "build_pack"}) {
        const J r = rig.send(rig.modern("prompts/get", {{"name", p}, {"arguments", {{"path", "ref.wav"}, {"key", "F minor"}, {"theme", "basses"}}}}));
        CHECK(r.contains("result"));
    }
    const J status1 = rig.ok("get_status", J::object());
    CHECK(status1["server"]["protocol_versions"][0] == "2026-07-28");
}

TEST_CASE("mcp: handshake sessions in revisions 2025-06-18 and 2025-11-25", "[mcp][protocol][legacy]") {
    for (const std::string version : {"2025-06-18", "2025-11-25"}) {
        INFO(version);
        Rig rig("legacy");
        const J early = rig.send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}});
        CHECK(early["error"]["code"] == -32600);
        const J init = rig.send({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "initialize"},
                                 {"params", {{"protocolVersion", version}, {"capabilities", J::object()}, {"clientInfo", {{"name", "codex-mcp-client"}, {"title", "Codex"}, {"version", "1"}}}}}});
        REQUIRE(init.contains("result"));
        CHECK(init["result"]["protocolVersion"] == version);
        CHECK(init["result"]["serverInfo"]["name"] == "zederia-mcp");
        CHECK(init["result"]["capabilities"].contains("tools"));
        CHECK_FALSE(init["result"].contains("resultType"));
        CHECK(rig.send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).is_null());
        const J tools = rig.send({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/list"}, {"params", {{"_meta", {{"progressToken", 0}}}}}});
        REQUIRE(tools.contains("result"));
        CHECK(tools["result"]["tools"].size() == 26);
        CHECK_FALSE(tools["result"].contains("resultType")); // that belongs to 2026-07-28
        CHECK_FALSE(tools["result"].contains("ttlMs"));
        CHECK(rig.send({{"jsonrpc", "2.0"}, {"id", 4}, {"method", "ping"}})["result"] == J::object());
        const J call = rig.send({{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"},
                                 {"params", {{"name", "theory"}, {"arguments", {{"action", "scale"}, {"key", "F minor"}}}}}});
        REQUIRE(call.contains("result"));
        CHECK(call["result"]["isError"] == false);
        CHECK(call["result"]["structuredContent"]["scale"]["name"] == "F minor");
        CHECK(rig.send({{"jsonrpc", "2.0"}, {"id", 6}, {"method", "server/discover"}}).contains("error"));
        // Undo labels carry the client's name from initialize.
        const J opened = rig.send({{"jsonrpc", "2.0"}, {"id", 7}, {"method", "tools/call"}, {"params", {{"name", "open_session"}, {"arguments", J::object()}}}});
        const std::string target = opened["result"]["structuredContent"]["target"];
        rig.send({{"jsonrpc", "2.0"}, {"id", 8}, {"method", "tools/call"},
                  {"params", {{"name", "set_params"}, {"arguments", {{"target", target}, {"params", {{{"id", "filter1.on"}, {"value", true}}}}}}}}});
        const J h = rig.send({{"jsonrpc", "2.0"}, {"id", 9}, {"method", "tools/call"}, {"params", {{"name", "history"}, {"arguments", {{"target", target}, {"action", "list"}}}}}});
        CHECK(h["result"]["structuredContent"]["steps"].back()["label"].get<std::string>().rfind("Codex: ", 0) == 0);
        CHECK(h["result"]["structuredContent"]["steps"].back()["origin"] == "Codex");
    }
    Rig rig("legacy_unknown");
    const J init = rig.send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", {{"protocolVersion", "2024-01-01"}, {"capabilities", J::object()}}}});
    CHECK(init["result"]["protocolVersion"] == "2025-11-25"); // the newest handshake revision it speaks
}

TEST_CASE("mcp: protocol errors", "[mcp][protocol][errors]") {
    Rig rig("errors");
    int status = 0;
    J bad = rig.modern("tools/list");
    bad["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "1900-01-01";
    J r = rig.send(bad, &status);
    CHECK(r["error"]["code"] == -32022);
    CHECK(r["error"]["data"]["supported"][0] == "2026-07-28");
    CHECK(r["error"]["data"]["requested"] == "1900-01-01");
    CHECK(status == 400);
    J noCaps = rig.modern("tools/list");
    noCaps["params"]["_meta"].erase("io.modelcontextprotocol/clientCapabilities");
    CHECK(rig.send(noCaps)["error"]["code"] == -32602);
    J badLevel = rig.modern("tools/list");
    badLevel["params"]["_meta"]["io.modelcontextprotocol/logLevel"] = "loud";
    CHECK(rig.send(badLevel)["error"]["code"] == -32602);
    CHECK(rig.send(rig.modern("ping"), &status)["error"]["code"] == -32601);
    CHECK(status == 404);
    CHECK(rig.send(rig.modern("tools/frobnicate"))["error"]["code"] == -32601);
    CHECK(rig.send(rig.modern("tools/call", {{"name", "no_such_tool"}}))["error"]["code"] == -32602);
    CHECK(rig.send(rig.modern("tools/call", {{"name", "rendr_audio"}}))["error"]["message"].get<std::string>().find("render_audio") != std::string::npos);
    CHECK(rig.send(rig.modern("tools/list", {{"cursor", "page2"}}))["error"]["code"] == -32602);
    CHECK(rig.send(J{{"jsonrpc", "1.0"}, {"id", 1}, {"method", "tools/list"}})["error"]["code"] == -32600);
    CHECK(rig.send(J{{"jsonrpc", "2.0"}, {"id", J::object()}, {"method", "tools/list"}})["error"]["code"] == -32600);
    CHECK(rig.send(J::array())["error"]["code"] == -32600);
    J legacyInMeta = rig.modern("tools/list");
    legacyInMeta["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2025-11-25"; // a handshake revision without its handshake
    CHECK(rig.send(legacyInMeta).contains("error"));
}

TEST_CASE("mcp: every tool runs and its result matches its output schema", "[mcp][tools]") {
    Rig rig("tools");
    const J status = rig.ok("get_status", J::object());
    CHECK(status["live_targets"].empty());
    const std::string t = rig.ok("open_session", {{"name", "test"}})["target"];
    CHECK(t.rfind("session:", 0) == 0);
    CHECK(rig.ok("list_targets", J::object())["targets"].size() == 1);
    CHECK(rig.ok("describe_synth", J::object())["modules"].size() > 10);
    const J filt = rig.ok("describe_synth", {{"module", "filter1"}});
    CHECK(filt["parameters"].size() > 5);
    CHECK(rig.ok("describe_synth", {{"module", "effect:delay"}})["parameters"].size() > 3);
    CHECK(rig.ok("describe_synth", {{"search", "unison detune"}})["parameters"].size() >= 3);
    const J applied = rig.ok("apply_patch", {{"target", t},
                                             {"patch", {{"meta", {{"name", "Glass Pluck"}}},
                                                        {"osc1", {{"unison", {{"voices", 3}, {"detune", 0.15}}}}},
                                                        {"filter1", {{"on", true}, {"type", "ladder_lp24"}, {"cutoff_hz", 1800}}},
                                                        {"env1", {{"attack_ms", 1}, {"decay_ms", 400}, {"sustain", 0.0}, {"release_ms", 200}}},
                                                        {"fx", {{"a", {{{"type", "chorus"}, {"mix", 0.3}}}}, {"master", {{{"type", "reverb"}, {"mix", 0.2}}}}}}}},
                                             {"label", "Pluck"}});
    CHECK(applied["applied"] == true);
    CHECK(applied["changes"].size() > 5);
    const J set = rig.ok("set_params", {{"target", t}, {"params", {{{"id", "filter1.cutoff"}, {"value", "900 Hz"}}, {{"id", "fx.a1.mix"}, {"value", 0.4}}, {{"id", "env1.decay"}, {"value", "1/8"}}}}});
    CHECK(set["params"][0]["value"].get<double>() == 900.0);
    CHECK(set["params"][2]["tempo_linked"] == "1/8");
    const J mod = rig.ok("set_modulation", {{"target", t}, {"routes", {{{"source", "env2"}, {"dest", "filter1.cutoff"}, {"amount", 0.5}, {"polarity", "uni"}},
                                                                        {{"source", "lfo1"}, {"dest", "osc1.position"}, {"amount", 0.3}, {"via", "modwheel"}}}}});
    CHECK(mod["routes"].size() == 2);
    CHECK(rig.ok("set_modulation", {{"target", t}, {"routes", {{{"action", "update"}, {"index", 2}, {"amount", -0.2}}}}})["routes"][1]["amount"].get<double>() == -0.2);
    CHECK(rig.ok("set_shape", {{"target", t}, {"owner", "lfo1"}, {"points", {{{"x", 0}, {"y", -1}}, {{"x", 0.5}, {"y", 1}, {"curve", 0.3}}, {{"x", 1}, {"y", -1}}}}})["kind"] == "lfo");
    CHECK(rig.ok("set_shape", {{"target", t}, {"owner", "env2"}, {"points", {{{"time_ms", 2}, {"level", 1}}, {{"time", "1/8"}, {"level", 0.3}}, {{"time_ms", 300}, {"level", 0}}}}, {"sustain_point", 1}})["kind"] == "envelope");
    CHECK(rig.ok("set_shape", {{"target", t}, {"owner", "osc1.warp_a"}, {"points", {{{"x", 0}, {"y", 0}}, {{"x", 1}, {"y", 1}, {"curve", 0.5}}}}})["kind"] == "warp");
    CHECK(rig.ok("set_shape", {{"target", t}, {"owner", "mod.1"}, {"points", {{{"x", 0}, {"y", 0}}, {{"x", 1}, {"y", 1}}}}})["kind"] == "route_curve");
    const J full = rig.ok("get_patch", {{"target", t}, {"form", "full"}});
    CHECK(full["patch"]["filter1"]["cutoff_hz"].get<double>() == 900.0);
    CHECK(rig.ok("get_patch", {{"target", t}, {"sections", {"filter1"}}})["patch"].size() == 1);
    const J table = rig.ok("create_wavetable", {{"name", "Odd Sweep"}, {"kind", "formula"}, {"domain", "harmonics"}, {"expression", "(h % 2) / h * (1 - y) + y / h"},
                                                {"frame_count", 8}, {"morph", "spectral"}, {"target", t}, {"osc", 1}, {"thumbnail", true}});
    CHECK(table["table_id"] == "user:Agent/Odd Sweep");
    CHECK(table["frames"] == 8);
    for (const J& spec : {J{{"name", "H"}, {"kind", "harmonics"}, {"harmonics", {1, 0.5, 0.33}}},
                          J{{"name", "S"}, {"kind", "shapes"}, {"shapes", {"sine", "saw", "pulse25"}}},
                          J{{"name", "F"}, {"kind", "frames"}, {"frames", {std::vector<double>(64, 0.5), std::vector<double>(32, -0.5)}}},
                          J{{"name", "R"}, {"kind", "recipe"}, {"frame_count", 2}, {"steps", {{{"op", "formula"}, {"domain", "wave"}, {"expr", "sin(tau*t)"}}}}}})
        CHECK(rig.ok("create_wavetable", spec)["frames"].get<int>() >= 1);
    CHECK(rig.ok("edit_wavetable", {{"table_id", "user:Agent/Odd Sweep"}, {"operations", {{{"op", "reverse"}}, {{"op", "process"}, {"name", "normalize_each"}}}}})["frames"] == 8);
    CHECK(rig.ok("edit_wavetable", {{"target", t}, {"osc", 1}, {"operations", {{{"op", "harmonic"}, {"name", "odd_only"}}}}, {"save_as", "Odd Again"}})["table_id"] == "user:Agent/Odd Again");
    CHECK(rig.ok("list_wavetables", {{"search", "odd"}})["total"].get<int>() >= 2);
    const J saved = rig.ok("save_preset", {{"target", t}, {"name", "Glass Pluck"}, {"tags", {"bright", "short"}}, {"type", "pluck"}, {"pack", "Tests"}});
    CHECK(saved["preset_id"] == "user:Agent/Tests/Glass Pluck"); // agents save in Presets/Agent
    CHECK(fs::exists(fs::path(saved["path"].get<std::string>())));
    const J listed = rig.ok("list_presets", {{"search", "tag:bright"}});
    CHECK(listed["total"] == 1);
    CHECK(rig.ok("tag_presets", {{"presets", {"user:Agent/Tests/Glass Pluck", "factory:Nope/Nothing"}}, {"add_tags", {"warm"}}, {"rating", 4}})["changed"] == 1);
    CHECK(rig.ok("list_presets", {{"tags", {"warm"}}})["total"] == 1);
    const std::string t2 = rig.ok("open_session", {{"from", "user:Agent/Tests/Glass Pluck"}})["target"];
    CHECK(rig.ok("load_preset", {{"target", t2}, {"preset", "user:Agent/Tests/Glass Pluck"}})["summary"]["name"] == "Glass Pluck");
    const J render = rig.ok("render_audio", {{"target", t}, {"notes", {{{"note", "C3"}, {"start", 0}, {"length", 0.5}}, {{"note", 55}, {"start", 0.5}, {"length", 0.5}, {"velocity", 100}}}},
                                             {"tempo", 120}, {"spectrogram", true}, {"name", "pluck_test"}});
    CHECK(fs::exists(fs::path(render["file"].get<std::string>())));
    CHECK(render["analysis"]["pitch"].contains("pitched"));
    CHECK(rig.ok("analyze_audio", {{"path", "Renders/pluck_test.wav"}})["analysis"]["level"]["peak_dbfs"].get<double>() < 0.0);
    CHECK(rig.ok("compare_audio", {{"a", "Renders/pluck_test.wav"}, {"b", render["file"]}})["comparison"]["distance"].get<double>() == 0.0);
    CHECK(rig.send(rig.modern("resources/read", {{"uri", render["uri"]}}))["result"]["contents"][0]["text"].get<std::string>().find("pluck_test.wav") != std::string::npos);
    const J theory = rig.ok("theory", {{"action", "progression"}, {"key", "F minor"}, {"symbols", {"i", "VI", "III", "VII"}}, {"bass", true}, {"beats_per_chord", 8}});
    CHECK(theory["chords"][1]["root"] == "Db");
    CHECK(rig.ok("theory", {{"action", "chord"}, {"symbols", {"Fm7", "Db/F"}}})["chords"].size() == 2);
    CHECK(rig.ok("theory", {{"action", "quantize"}, {"key", "C major"}, {"notes", {61, "F#4"}}})["notes"][1]["to"] == 67);
    CHECK(rig.ok("theory", {{"action", "scales"}})["scales"].size() > 10);
    const J clip = rig.ok("write_clip", {{"name", "chords"}, {"bars", 8}, {"tempo", 124}, {"key", "F minor"}, {"notes", theory["clip_notes"]}, {"target", t}, {"slot", 1}});
    const std::string clipId = clip["clip_id"];
    CHECK(clip["loaded_into"]["slot"] == 1);
    CHECK(rig.ok("get_clip", {{"clip_id", clipId}})["clip"]["notes"].size() == theory["clip_notes"].size());
    CHECK(rig.ok("get_clip", {{"target", t}, {"slot", 1}})["clip"]["bars"] == 8.0);
    for (const char* tail : {"fold", "cut", "separate"}) {
        const J loop = rig.ok("render_audio", {{"target", t}, {"clip", clipId}, {"tail", tail}, {"max_tail_seconds", 2}});
        CHECK(loop["loop"]["bars"] == 8.0);
        CHECK(loop["duration_s"].get<double>() == Catch::Approx(8 * 4 * 60.0 / 124.0).margin(0.001));
        CHECK(fs::exists(fs::path(loop["midi_file"].get<std::string>())));
        CHECK(loop.contains("tail_file") == (std::string(tail) == "separate"));
        CHECK(fs::path(loop["file"].get<std::string>()).filename().string().rfind("ZED_chords_GlassPluck_Fmin_124bpm", 0) == 0);
    }
    const J pack = rig.ok("export_pack", {{"name", "Test Pack"}, {"presets", {"user:Agent/Tests/Glass Pluck", t}}, {"clips", {clipId}}, {"keys", {"F minor", "G minor"}},
                                          {"one_shots", {{"notes", {"C2", "C3"}}, {"length_s", 0.5}}},
                                          {"multi_samples", {{"notes", {"C3"}}, {"velocities", {64, 127}}, {"length_s", 0.5}}}});
    CHECK(pack["count"] == 2 * (2 + 2 + 2));
    std::set<std::string> names;
    for (const auto& f : pack["files"])
        names.insert(fs::path(f["file"].get<std::string>()).filename().string());
    CHECK(names.count("ZED_chords_GlassPluck_Gmin_124bpm.wav"));
    CHECK(names.count("ZED_oneshot_GlassPluck_C2.wav"));
    CHECK(names.count("ZED_GlassPluck_C3_v127.wav"));
    const J h = rig.ok("history", {{"target", t}, {"action", "list"}});
    CHECK(h["steps"].size() > 8);
    CHECK(h["steps"].back()["label"].get<std::string>().rfind("Tests: ", 0) == 0);
    CHECK(rig.ok("history", {{"target", t}, {"action", "undo"}})["can_redo"] == true);
    CHECK(rig.ok("history", {{"target", t}, {"action", "redo"}})["can_redo"] == false);
    rig.ok("history", {{"target", t}, {"action", "revert_agent_session"}});
    CHECK(rig.ok("get_patch", {{"target", t}})["patch"].value("filter1", J::object()).empty()); // back to init
    rig.ok("history", {{"target", t}, {"action", "jump"}, {"step", 2}});
    CHECK(rig.ok("close_session", {{"target", t}})["closed"] == true);
    rig.ok("close_session", {{"target", t2}});
    CHECK(rig.server.outputSchemaFailures().empty());
}

TEST_CASE("mcp: invalid arguments come back with paths and fixes", "[mcp][tools][errors]") {
    Rig rig("badargs");
    const std::string t = rig.ok("open_session", J::object())["target"];
    J e = rig.error("set_params", {{"target", t}, {"params", {{{"id", "filter1.cutof"}, {"value", 900}}}}});
    CHECK(anyPath(e, "/params/0/id"));
    CHECK(anyText(e, "did you mean \\\"filter1.cutoff\\\""));
    e = rig.error("set_params", {{"target", t}, {"params", {{{"id", "filter1.type"}, {"value", "ladder_lp25"}}}}});
    CHECK(anyPath(e, "/params/0/value"));
    e = rig.error("set_params", {{"target", t}, {"params", {{{"id", "fx.a1.mix"}, {"value", 0.5}}}}});
    CHECK(anyText(e, "empty effect slot"));
    e = rig.error("set_params", {{"target", t}, {"paramz", J::array()}});
    CHECK(anyPath(e, "/paramz"));
    CHECK(anyPath(e, "/params"));
    e = rig.error("apply_patch", {{"target", t}, {"patch", {{"filter1", {{"cutoff_hz", "loud"}}}}}});
    CHECK(anyPath(e, "/patch/filter1/cutoff_hz"));
    e = rig.error("apply_patch", {{"target", t}, {"patch", {{"filter1", {{"on", true}}}}}, {"mode", "mergee"}});
    CHECK(anyText(e, "did you mean \\\"merge\\\""));
    e = rig.error("set_modulation", {{"target", t}, {"routes", {{{"source", "lfo9"}, {"dest", "filter1.cutoff"}}}}});
    CHECK(anyPath(e, "/routes/0/source"));
    e = rig.error("set_modulation", {{"target", t}, {"routes", {{{"source", "lfo1"}, {"dest", "global.mode"}}}}});
    CHECK(anyText(e, "cannot be modulated"));
    e = rig.error("set_shape", {{"target", t}, {"owner", "lfo12"}, {"points", {{{"x", 0}, {"y", 0}}}}});
    CHECK(anyPath(e, "/owner"));
    e = rig.error("get_patch", {{"target", "session:deadbeef"}});
    CHECK(anyText(e, "open_session"));
    e = rig.error("get_patch", {{"target", "patch-1"}});
    CHECK(anyPath(e, "/target"));
    e = rig.error("create_wavetable", {{"name", "X"}, {"kind", "formula"}, {"expression", "sine(tau*t)"}});
    CHECK(anyText(e, "sin"));
    e = rig.error("write_clip", {{"name", "c"}, {"bars", 1}, {"tempo", 120}, {"notes", {{{"start", 5}, {"length", 1}, {"key", 60}}}}});
    CHECK(anyPath(e, "/notes/0/start"));
    e = rig.error("render_audio", {{"target", t}, {"notes", {{{"note", 60}, {"start", 0}, {"length", 400}}}}});
    CHECK(anyText(e, "at most 120 s"));
    e = rig.error("audition", {{"target", t}, {"notes", {{{"note", 60}, {"start", 0}, {"length", 1}}}}});
    CHECK(anyText(e, "live instance"));
    e = rig.error("theory", {{"action", "scale"}, {"key", "H minor"}});
    CHECK(anyPath(e, "/key"));
    e = rig.error("open_session", {{"from", "user:Nothing/Here"}});
    CHECK(anyPath(e, "/from"));
}

TEST_CASE("mcp: apply_patch clamps and reports, dry runs change nothing", "[mcp][tools]") {
    Rig rig("apply");
    const std::string t = rig.ok("open_session", J::object())["target"];
    const J r = rig.ok("apply_patch", {{"target", t}, {"patch", {{"filter1", {{"on", true}, {"cutoff_hz", 99999}, {"cutof_hz", 3}}}}}});
    bool clamped = false, unknown = false;
    for (const auto& w : r["warnings"]) {
        clamped = clamped || w.value("path", "") == "/patch/filter1/cutoff_hz";
        unknown = unknown || w.value("code", "") == "unknown_field";
    }
    CHECK(clamped);
    CHECK(unknown);
    const double top = rig.ok("get_patch", {{"target", t}, {"form", "full"}})["patch"]["filter1"]["cutoff_hz"];
    CHECK(top < 99999.0);
    const J dry = rig.ok("apply_patch", {{"target", t}, {"patch", {{"filter1", {{"cutoff_hz", 500}}}}}, {"dry_run", true}});
    CHECK(dry["applied"] == false);
    CHECK(dry["changes"].size() == 1);
    CHECK(rig.ok("get_patch", {{"target", t}, {"form", "full"}})["patch"]["filter1"]["cutoff_hz"].get<double>() == top);
    rig.ok("apply_patch", {{"target", t}, {"patch", {{"meta", {{"name", "Replaced"}}}}}, {"mode", "replace"}});
    CHECK(rig.ok("get_patch", {{"target", t}})["patch"].value("filter1", J::object()).empty());
}

TEST_CASE("mcp: the file sandbox", "[mcp][safety][sandbox]") {
    Rig rig("sandbox");
    const fs::path user = rig.root / "user";
    std::ofstream(user / "ok.wav") << "x";
    std::ofstream(rig.root / "outside" / "secret.wav") << "x";
    const std::vector<fs::path> roots = {user};
    const auto check = [&](const std::string& p) { return zmcp::sandboxPath(p, user, roots, {".wav"}); };
    CHECK(check("ok.wav").ok);
    CHECK(check((user / "ok.wav").string()).ok);
    CHECK(check("Renders/new.wav").ok); // a file that does not exist yet, inside the root
    for (const std::string& bad : std::vector<std::string>{"../outside/secret.wav", "Renders/../../outside/secret.wav", (rig.root / "outside" / "secret.wav").string(),
                                  "\\\\server\\share\\x.wav", "//server/share/x.wav", "\\\\?\\C:\\x.wav", "\\\\.\\pipe\\x.wav", "ok.wav:hidden.wav",
                                  "CON.wav", "nul.wav", "com1.wav", "dir./x.wav", "dir /x.wav", "ok.exe", "ok", std::string("a\0b.wav", 7), ""}) {
        INFO(bad);
        const auto r = check(bad);
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.fix.empty());
    }
#ifdef _WIN32
    // A junction inside the root that points outside is resolved and refused.
    const std::string cmd = "cmd /c mklink /J \"" + (user / "jump").string() + "\" \"" + (rig.root / "outside").string() + "\" >nul 2>nul";
    if (std::system(cmd.c_str()) == 0 && fs::exists(user / "jump" / "secret.wav")) {
        CHECK_FALSE(check("jump/secret.wav").ok);
    } else {
        WARN("could not create a junction; that case was not checked");
    }
#endif
    // Through the tools.
    auto e = rig.error("analyze_audio", {{"path", "../outside/secret.wav"}});
    CHECK(anyText(e, "\\\"..\\\" is not allowed"));
    e = rig.error("analyze_audio", {{"path", (rig.root / "outside" / "secret.wav").string()}});
    CHECK(anyText(e, "AGENT page"));
    const std::string t = rig.ok("open_session", J::object())["target"];
    e = rig.error("save_preset", {{"target", t}, {"name", "../evil"}});
    CHECK(anyPath(e, "/name"));
    e = rig.error("save_preset", {{"target", t}, {"name", "x"}, {"pack", "a/b"}});
    CHECK(anyPath(e, "/pack"));
    e = rig.error("render_audio", {{"target", t}, {"notes", {{{"note", 60}, {"start", 0}, {"length", 0.1}}}}, {"folder", "../../outside"}});
    CHECK(anyPath(e, "/folder"));
    e = rig.error("create_wavetable", {{"name", "X"}, {"kind", "audio"}, {"path", "C:/Windows/win.ini"}});
    CHECK(anyPath(e, "/path"));
    // Permissions: writing switched off on the AGENT page.
    zed::link::Permissions p;
    p.writeZederia = false;
    std::string error;
    REQUIRE(zed::link::savePermissions(p, error));
    e = rig.error("save_preset", {{"target", t}, {"name", "Blocked"}});
    CHECK(anyText(e, "switched off"));
    // Clips and renders too, the default Renders folder included: before the pre-milestone-7 audit
    // both wrote their files while writing was off.
    e = rig.error("write_clip", {{"name", "blocked"}, {"bars", 1}, {"tempo", 120}, {"notes", {{{"start", 0}, {"length", 1}, {"key", 60}}}}});
    CHECK(anyText(e, "switched off"));
    e = rig.error("render_audio", {{"target", t}, {"notes", {{{"note", 60}, {"start", 0}, {"length", 0.1}}}}});
    CHECK(anyText(e, "switched off"));
    for (const char* sub : {"Clips", "Renders"}) {
        INFO(sub);
        CHECK((!fs::exists(user / sub) || fs::is_empty(user / sub) || fs::is_empty(user / sub / "Agent")));
    }
    // An allowed read folder opens the outside file.
    p = zed::link::Permissions{};
    p.readFolders = {(rig.root / "outside").string()};
    REQUIRE(zed::link::savePermissions(p, error));
    CHECK(rig.bridge.resolveRead((rig.root / "outside" / "secret.wav").string(), {".wav"}, "/path") ==
          fs::weakly_canonical(rig.root / "outside" / "secret.wav"));
    p.readSamples = false;
    REQUIRE(zed::link::savePermissions(p, error));
    CHECK_THROWS_AS(rig.bridge.resolveRead((rig.root / "outside" / "secret.wav").string(), {".wav"}, "/path"), zmcp::ToolError);
}

TEST_CASE("mcp: long renders report progress and stop when cancelled", "[mcp][cancel]") {
    Rig rig("cancel");
    const std::string t = rig.ok("open_session", J::object())["target"];
    // A heavy patch: three oscillators at unison 16, so the render takes many seconds.
    rig.ok("apply_patch", {{"target", t}, {"patch", {{"osc1", {{"unison", {{"voices", 16}}}}}, {"osc2", {{"on", true}, {"unison", {{"voices", 16}}}}},
                                                    {"osc3", {{"on", true}, {"unison", {{"voices", 16}}}}}, {"filter1", {{"on", true}}}, {"filter2", {{"on", true}}}}}});
    J notes = J::array();
    for (int i = 0; i < 8; ++i)
        notes.push_back({{"note", 48 + i * 3}, {"start", 0}, {"length", 110}});
    std::vector<double> progress;
    std::atomic<bool> cancel{false};
    zmcp::RequestInfo info;
    info.modern = true;
    info.clientName = "Tests";
    info.progressToken = "render-1";
    info.notify = [&](const J& n) {
        CHECK(n["method"] == "notifications/progress");
        CHECK(n["params"]["progressToken"] == "render-1");
        progress.push_back(n["params"]["progress"].get<double>());
        if (progress.size() >= 3)
            cancel = true;
    };
    info.cancelled = [&] { return cancel.load(); };
    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS_AS(rig.server.callTool("render_audio", {{"target", t}, {"notes", notes}, {"units", "seconds"}, {"name", "cancelled"}}, info), zmcp::Cancelled);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    REQUIRE(progress.size() >= 3);
    for (std::size_t i = 1; i < progress.size(); ++i)
        CHECK(progress[i] > progress[i - 1]); // progress only increases
    CHECK(progress.back() < 0.5);
    CHECK(rig.server.cancelledCount() == 1);
    CHECK_FALSE(fs::exists(rig.root / "user" / "Renders" / "cancelled.wav"));
    UNSCOPED_INFO("cancelled after " << seconds << " s");
    // Through handle(): a cancelled request gets no response at all.
    zmcp::RequestInfo late;
    late.cancelled = [] { return true; };
    J req = rig.modern("tools/call", {{"name", "render_audio"}, {"arguments", {{"target", t}, {"notes", notes}, {"units", "seconds"}}}});
    zmcp::LegacyChannel ch;
    J reply;
    try {
        reply = rig.server.handle(req, &ch, late);
    } catch (const zmcp::Cancelled&) {
        reply = nullptr;
    }
    CHECK(reply.is_null());
}

// Fixes from the audit before milestone 7 (2026-11-22, docs/STATUS.md).
TEST_CASE("mcp: clips, rates, paging, tails and pack limits from the pre-release audit", "[mcp][tools][audit]") {
    Rig rig("audit");
    const fs::path clips = rig.root / "user" / "Clips" / "Agent";
    const auto clipFiles = [&] {
        std::size_t n = 0;
        std::error_code ec;
        if (fs::exists(clips, ec))
            for (const auto& e : fs::directory_iterator(clips, ec))
                n += e.is_regular_file() ? 1 : 0;
        return n;
    };
    const J oneNote = J::array({{{"start", 0}, {"length", 1}, {"key", 60}}});
    // A clip shorter than the clip player's 1/16 of a beat is refused (it rendered as a loop of no
    // samples and divided by zero).
    auto e = rig.error("write_clip", {{"name", "tiny"}, {"bars", 0.00001}, {"beats_per_bar", 1}, {"tempo", 400},
                                      {"notes", {{{"start", 0}, {"length", 0.000001}, {"key", 60}}}}});
    CHECK(anyText(e, "1/16"));
    // A refused target or slot writes no clip.
    e = rig.error("write_clip", {{"name", "lost"}, {"bars", 1}, {"tempo", 120}, {"notes", oneNote}, {"slot", 2}});
    CHECK(anyPath(e, "/slot"));
    e = rig.error("write_clip", {{"name", "lost"}, {"bars", 1}, {"tempo", 120}, {"notes", oneNote}, {"target", "session:00000000"}});
    CHECK(anyPath(e, "/target"));
    CHECK(clipFiles() == 0);
    // Summaries say fractional bars as they are.
    const J half = rig.call("write_clip", {{"name", "half"}, {"bars", 1.5}, {"tempo", 120}, {"notes", oneNote}});
    CHECK(half["content"][0]["text"].get<std::string>().find("1.5 bar(s)") != std::string::npos);
    CHECK(clipFiles() == 1);

    // A note length for an LFO rate switches the LFO to synced at that division.
    const std::string t = rig.ok("open_session", J::object())["target"];
    const J set = rig.ok("set_params", {{"target", t}, {"params", {{{"id", "lfo2.rate"}, {"value", "1/8D"}}}}});
    CHECK(set["params"][0]["tempo_linked"] == "1/8D");
    const J lfo = rig.ok("get_patch", {{"target", t}, {"form", "full"}, {"sections", {"lfo2"}}})["patch"]["lfo2"];
    CHECK(lfo["sync"] == true);
    CHECK(lfo["division"] == "1/8D");
    e = rig.error("set_params", {{"target", t}, {"params", {{{"id", "lfo2.rate"}, {"value", "1/7"}}}}});
    CHECK(anyPath(e, "/params/0/value"));

    // Error texts end each sentence once.
    const J bad = rig.call("apply_patch", {{"target", t}, {"patch", {{"filter1", {{"cutoff_hz", "not a number"}}}}}});
    REQUIRE(bad["isError"] == true);
    CHECK(bad["content"][0]["text"].get<std::string>().find("..") == std::string::npos);

    // An offset beyond int's range is past the end, not a negative index.
    CHECK(rig.ok("list_presets", {{"offset", 3000000000LL}})["presets"].empty());

    // A quiet render ends where the analysis measures its tail to, so the tail reads complete.
    rig.ok("set_params", {{"target", t}, {"params", {{{"id", "global.master"}, {"value", "-40 dB"}}}}});
    const J quiet = rig.ok("render_audio", {{"target", t}, {"units", "seconds"}, {"notes", {{{"note", 60}, {"start", 0}, {"length", 0.2}}}}});
    CHECK(quiet["analysis"]["time"]["tail_complete"] == true);

    // Each file of a pack is one render, held to the render limit (64 bars at 20 bpm is 768 s).
    const std::string longClip = rig.ok("write_clip", {{"name", "long"}, {"bars", 64}, {"tempo", 20}, {"notes", oneNote}})["clip_id"];
    e = rig.error("export_pack", {{"name", "Too Long"}, {"presets", {t}}, {"clips", {longClip}}});
    CHECK(anyPath(e, "/clips/0"));
}

TEST_CASE("mcp: closing a session while other requests use it is safe", "[mcp][tools][audit][sessions]") {
    // Requests that found the session keep it alive until they finish; later ones are told it is
    // gone. Before the audit fix, close_session could free a session another request had locked.
    Rig rig("close_race");
    for (int round = 0; round < 8; ++round) {
        const std::string t = rig.ok("open_session", J::object())["target"];
        std::atomic<bool> stop{false};
        std::atomic<int> answered{0};
        std::vector<std::thread> workers;
        for (int w = 0; w < 4; ++w)
            workers.emplace_back([&, w] {
                for (int i = 0; !stop.load() && i < 200; ++i) {
                    const J req = {{"jsonrpc", "2.0"}, {"id", w * 1000 + i}, {"method", "tools/call"},
                                   {"params", {{"name", "set_params"}, {"arguments", {{"target", t}, {"params", {{{"id", "filter1.cutoff"}, {"value", 500 + i}}}}}}, {"_meta", Rig::meta()}}}};
                    zmcp::RequestInfo info;
                    const J r = rig.server.handle(req, nullptr, info);
                    if (r.contains("result"))
                        ++answered;
                }
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        rig.ok("close_session", {{"target", t}});
        stop = true;
        for (auto& w : workers)
            w.join();
        CHECK(answered.load() > 0);
        CHECK(rig.call("get_patch", {{"target", t}})["isError"] == true);
    }
}

TEST_CASE("mcp: agents save presets in Presets/Agent unless the user names another folder", "[mcp][tools][presets]") {
    Rig rig("presets");
    const std::string t = rig.ok("open_session", J::object())["target"];
    const fs::path presets = rig.root / "user" / "Presets";
    // By default, and with a pack: inside Presets/Agent, never beside the user's own folders (an
    // agent asked for a kick used to save it in the user's own Presets/808 by passing pack "808").
    J saved = rig.ok("save_preset", {{"target", t}, {"name", "Basement Kick"}, {"type", "percussion"}});
    CHECK(saved["preset_id"] == "user:Agent/Basement Kick");
    CHECK(saved["folder"] == "Agent");
    CHECK(fs::exists(presets / "Agent" / "Basement Kick.zpreset"));
    saved = rig.ok("save_preset", {{"target", t}, {"name", "Basement Kick"}, {"pack", "808"}});
    CHECK(saved["preset_id"] == "user:Agent/808/Basement Kick");
    CHECK(fs::exists(presets / "Agent" / "808" / "Basement Kick.zpreset"));
    CHECK_FALSE(fs::exists(presets / "808"));
    // An existing preset stays unless overwrite is given.
    CHECK(anyText(rig.error("save_preset", {{"target", t}, {"name", "Basement Kick"}}), "already exists"));
    CHECK(rig.ok("save_preset", {{"target", t}, {"name", "Basement Kick"}, {"overwrite", true}})["preset_id"] == "user:Agent/Basement Kick");
    // Another folder only when it is named, which the tool keeps for when the user asked for it.
    const auto* def = rig.server.findTool("save_preset");
    REQUIRE(def != nullptr);
    CHECK(def->description.find("only when the user asked") != std::string::npos);
    CHECK(def->input["properties"]["folder"]["description"].get<std::string>().find("only when the user asked") != std::string::npos);
    saved = rig.ok("save_preset", {{"target", t}, {"name", "Basement Kick"}, {"folder", "808\\Kicks"}});
    CHECK(saved["preset_id"] == "user:808/Kicks/Basement Kick");
    CHECK(fs::exists(presets / "808" / "Kicks" / "Basement Kick.zpreset"));
    for (const char* bad : {"../Elsewhere", "C:/Users", "a/b/c/d/e", "", "x*y"}) {
        INFO(bad);
        CHECK(anyPath(rig.error("save_preset", {{"target", t}, {"name", "X"}, {"folder", bad}}), "/folder"));
    }
    CHECK_FALSE(fs::exists(rig.root / "user" / "Elsewhere"));
}

TEST_CASE("mcp: limits and safe file names from the milestone 7 review of the bridge", "[mcp][safety][review]") {
    Rig rig("review");
    const std::string t = rig.ok("open_session", J::object())["target"];
    const J note = J::array({{{"note", 60}, {"start", 0}, {"length", 0.25}}});

    // JSON nested deeper than 64 levels is refused before parsing; strings do not count.
    const std::string bracketsInString = R"({"a":[1,{"b":"[[[["}]})";
    const std::string escapedQuote = R"(["a\"[[[",["b"]])";
    CHECK(zed::jsonNestingDepth(bracketsInString) == 3);
    CHECK(zed::jsonNestingDepth(escapedQuote) == 2);
    CHECK(zed::jsonNestingAllowed(std::string(64, '[') + std::string(64, ']')));
    CHECK_FALSE(zed::jsonNestingAllowed(std::string(65, '[') + std::string(65, ']')));

    // A very long unknown parameter id is answered at once, with no suggestion; a slip still gets one.
    const auto started = std::chrono::steady_clock::now();
    J e = rig.error("set_params", {{"target", t}, {"params", {{{"id", std::string(200000, 'q')}, {"value", 1}}}}});
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
    CHECK_FALSE(anyText(e, "did you mean"));
    CHECK(zmcp::closest("filter1.cutof", {"osc1.level", "filter1.cutoff"}) == "filter1.cutoff");
    CHECK(zmcp::closest(std::string(80, 'a'), {std::string(80, 'a') + "b"}).empty());

    // Renders given the same name at the same time get a file each.
    std::vector<J> twins(2);
    std::vector<std::thread> threads;
    for (int i = 0; i < 2; ++i)
        threads.emplace_back([&, i] {
            zmcp::RequestInfo info;
            zmcp::LegacyChannel ch;
            J params = {{"name", "render_audio"}, {"arguments", {{"target", t}, {"notes", note}, {"units", "seconds"}, {"name", "twin"}}}};
            params["_meta"] = Rig::meta();
            const J request = {{"jsonrpc", "2.0"}, {"id", 900 + i}, {"method", "tools/call"}, {"params", params}};
            twins[static_cast<std::size_t>(i)] = rig.server.handle(request, &ch, info);
        });
    for (auto& th : threads)
        th.join();
    std::set<std::string> files;
    for (const auto& r : twins) {
        INFO(r.dump().substr(0, 1000));
        REQUIRE(r.contains("result"));
        REQUIRE(r["result"].value("isError", true) == false);
        files.insert(r["result"]["structuredContent"]["file"].get<std::string>());
    }
    CHECK(files.size() == 2);
    CHECK(fs::exists(rig.root / "user" / "Renders" / "twin.wav"));
    CHECK(fs::exists(rig.root / "user" / "Renders" / "twin_2.wav"));

    // A render in the Renders folder is a resource; one in another folder links to its own file.
    const J linked = rig.ok("render_audio", {{"target", t}, {"notes", note}, {"units", "seconds"}, {"name", "linked"}});
    CHECK(linked["uri"] == "zederia://renders/linked");
    const J resource = rig.send(rig.modern("resources/read", {{"uri", "zederia://renders/linked"}}));
    REQUIRE(resource.contains("result"));
    CHECK(resource["result"]["contents"][0]["text"].get<std::string>().find("zederia.render") != std::string::npos);
    const J elsewhere = rig.ok("render_audio", {{"target", t}, {"notes", note}, {"units", "seconds"}, {"name", "linked take"}, {"folder", "Takes"}});
    const std::string uri = elsewhere["uri"];
    CHECK(uri.rfind("file:///", 0) == 0);
    CHECK(uri.find("/Takes/linked%20take.wav") != std::string::npos);
    CHECK(uri.find('\\') == std::string::npos);
    CHECK(fs::exists(rig.root / "user" / "Renders" / "Takes" / "linked take.wav"));

    // Audio too large to analyse is refused before it is read.
    const fs::path huge = rig.root / "user" / "Renders" / "huge.wav";
    std::ofstream(huge, std::ios::binary) << "RIFF";
    fs::resize_file(huge, 401ull * 1024 * 1024);
    CHECK(anyText(rig.error("analyze_audio", {{"path", "Renders/huge.wav"}}), "larger than 400 MB"));
    CHECK(anyText(rig.error("compare_audio", {{"a", "Renders/linked.wav"}, {"b", "Renders/huge.wav"}}), "larger than 400 MB"));
    fs::remove(huge);

    // A damaged permissions file switches everything off until the AGENT page saves them again.
    const fs::path permissions = rig.root / "settings" / "agent.json";
    std::ofstream(permissions, std::ios::binary) << "{ \"read_patch\": true, ";
    auto p = zed::link::loadPermissions();
    CHECK_FALSE(p.readPatch);
    CHECK_FALSE(p.editPatch);
    CHECK_FALSE(p.writeZederia);
    CHECK_FALSE(p.writeElsewhere);
    CHECK_FALSE(p.audition);
    CHECK_FALSE(p.readSamples);
    CHECK(rig.call("render_audio", {{"target", t}, {"notes", note}, {"units", "seconds"}, {"name", "refused"}})["isError"] == true);
    CHECK_FALSE(fs::exists(rig.root / "user" / "Renders" / "refused.wav"));
    std::ofstream(permissions, std::ios::binary) << std::string(200, '[') + std::string(200, ']');
    CHECK_FALSE(zed::link::loadPermissions().editPatch);
    fs::remove(permissions);
    CHECK(zed::link::loadPermissions() == zed::link::Permissions{});

    // Tokens and session ids come from the system's secure random numbers.
    std::string a, b;
    REQUIRE(zed::link::secureRandomHex(16, a));
    REQUIRE(zed::link::secureRandomHex(16, b));
    CHECK(a.size() == 32);
    CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(a != b);

    // A file is created only while its name is free.
    bool taken = false;
    const fs::path fresh = rig.root / "user" / "fresh.txt";
    CHECK(zed::link::createNewFile(fresh.string(), taken));
    CHECK_FALSE(taken);
    CHECK_FALSE(zed::link::createNewFile(fresh.string(), taken));
    CHECK(taken);
    CHECK_FALSE(zed::link::createNewFile((rig.root / "missing" / "x.txt").string(), taken));
    CHECK_FALSE(taken);
}
