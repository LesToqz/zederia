#include "../support/TestUtil.h"
#include "zederia/params/ParamDocs.h"
#include "zederia/params/Registry.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>

using namespace zed;
using Catch::Approx;

TEST_CASE("registry: ids are unique, well formed and belong to their module", "[registry]") {
    const auto& reg = Registry::get();
    REQUIRE(reg.size() > 500);
    const std::regex shape("^[a-z][a-z0-9_]*(\\.[a-z0-9_]+)+$");
    std::set<std::string> seen;
    for (const auto& p : reg.params()) {
        INFO(p.id);
        CHECK(std::regex_match(p.id, shape));
        CHECK(seen.insert(p.id).second);
        CHECK(p.id.rfind(p.module + ".", 0) == 0);
        CHECK(reg.indexOf(p.id) == p.index);
        CHECK(&reg.at(p.index) == &p);
    }
    CHECK(reg.indexOf("no.such.parameter") == -1);
    CHECK(reg.find("nope") == nullptr);
}

TEST_CASE("registry: host ids are stable hashes without collisions", "[registry]") {
    const auto& reg = Registry::get();
    // The hash is the one the plug-in framework applies to a parameter's string id.
    CHECK(hostIdFor("a") == 97u);
    CHECK(hostIdFor("ab") == 97u * 31u + 98u);
    // Reference values computed independently; they must never change, or saved automation breaks.
    CHECK(hostIdFor("osc1.position") == 117141125u);
    CHECK(hostIdFor("filter1.cutoff") == 53541826u);
    CHECK(hostIdFor("fx.m8.p16") == 56351790u);
    std::set<std::uint32_t> seen;
    for (const auto& p : reg.params()) {
        INFO(p.id);
        CHECK(p.hostId == hostIdFor(p.id));
        CHECK(p.hostId <= 0x7fffffffu);
        CHECK(seen.insert(p.hostId).second); // a collision would make two parameters one for the host
        REQUIRE(reg.findByHostId(p.hostId) != nullptr);
        CHECK(reg.findByHostId(p.hostId)->id == p.id);
    }
}

TEST_CASE("registry: every parameter is documented and has a sound range", "[registry]") {
    const auto& reg = Registry::get();
    for (const auto& p : reg.params()) {
        INFO(p.id);
        CHECK(!p.name.empty());
        REQUIRE(p.description.size() > 8);
        CHECK(p.description.back() == '.');
        CHECK(p.min < p.max);
        CHECK(p.def >= p.min);
        CHECK(p.def <= p.max);
        if (p.skew == Skew::Log)
            CHECK(p.min > 0.0);
        if (p.type == ParamType::Choice) {
            CHECK(p.choices.size() >= 2);
            CHECK(p.choices.size() == static_cast<std::size_t>(p.max) + 1);
            CHECK(std::set<std::string>(p.choices.begin(), p.choices.end()).size() == p.choices.size());
        } else {
            CHECK(p.choices.empty());
        }
        if (p.type == ParamType::Bool) {
            CHECK(p.min == 0.0);
            CHECK(p.max == 1.0);
        }
        if (p.stepped()) {
            CHECK(p.smoothing == Smoothing::None);
            CHECK_FALSE(p.modulatable);
            CHECK(p.def == std::round(p.def));
        }
    }
}

TEST_CASE("registry: normalized values map onto the range and back", "[registry]") {
    const auto& reg = Registry::get();
    std::vector<const ParamDef*> all;
    for (const auto& p : reg.params())
        all.push_back(&p);
    for (const auto& e : reg.effectTypes())
        for (const auto& p : e.params)
            all.push_back(&p);
    for (const ParamDef* p : all) {
        INFO(p->id);
        CHECK(p->fromNormalized(0.0) == p->min);
        CHECK(p->fromNormalized(1.0) == Approx(p->max).epsilon(1e-12));
        CHECK(p->fromNormalized(-3.0) == p->min);
        CHECK(p->fromNormalized(7.0) == Approx(p->max).epsilon(1e-12));
        CHECK(p->clamp(std::nan("")) == p->def);
        CHECK(p->clamp(1e300) == p->max);
        CHECK(p->clamp(-1e300) == p->min);
        for (const double n : {0.0, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0}) {
            const double plain = p->fromNormalized(n);
            CHECK(plain >= p->min);
            CHECK(plain <= p->max);
            if (p->stepped()) {
                CHECK(plain == std::round(plain));
                CHECK(p->fromNormalized(p->toNormalized(plain)) == plain);
            } else {
                CHECK(p->toNormalized(plain) == Approx(n).margin(1e-9));
            }
        }
        CHECK(!p->format(p->def).empty());
    }
}

TEST_CASE("registry: patch pointers are unique and generic slot parameters have none", "[registry]") {
    const auto& reg = Registry::get();
    std::set<std::string> seen;
    for (const auto& p : reg.params()) {
        INFO(p.id);
        int lane = 0, slot = 0, k = 0;
        if (reg.fxSlotParamAt(p.index, lane, slot, k)) {
            CHECK(p.pointer.empty());
            CHECK(reg.fxSlotParam(lane, slot, k) == p.index);
            CHECK(p.id == "fx." + std::string(fxLaneIdPrefix(lane)) + std::to_string(slot + 1) + ".p" + std::to_string(k + 1));
        } else {
            REQUIRE(!p.pointer.empty());
            CHECK(p.pointer[0] == '/');
            CHECK(seen.insert(p.pointer).second);
        }
    }
    CHECK(reg.at(reg.fxSlotOn(1, 2)).id == "fx.b3.on");
    CHECK(reg.at(reg.fxSlotMix(2, 7)).id == "fx.m8.mix");
    CHECK(reg.at(reg.modAmount(11)).id == "mod.12.amount");
    CHECK(reg.at(reg.macroValue(7)).id == "macro8.value");
    CHECK(reg.at(reg.macroBipolar(0)).id == "macro1.bipolar");
}

TEST_CASE("registry: follows the id conventions and units of the plan", "[registry]") {
    const auto& reg = Registry::get();
    for (const char* id : {"osc1.position", "osc2.unison.detune", "filter2.cutoff", "lfo3.rate", "fx.a3.mix",
                           "mod.12.amount", "mod.64.amount", "env4.release", "lfo8.rate", "flux2.rate",
                           "macro8.value", "fx.m8.p16", "arp.rate", "chord.strum", "clip.slot", "sub.level",
                           "texture.pitch", "voice.drift", "global.master"})
        CHECK(reg.find(id) != nullptr);
    // Real units in the patch document.
    CHECK(reg.find("filter1.cutoff")->pointer == "/filter1/cutoff_hz");
    CHECK(reg.find("env1.attack")->pointer == "/env1/attack_ms");
    CHECK(reg.find("osc1.level")->pointer == "/osc1/level_db");
    CHECK(reg.find("global.glide")->pointer == "/global/glide_ms");
    CHECK(reg.find("global.master")->pointer == "/global/master_db");
    CHECK(reg.find("global.bend_up")->pointer == "/global/bend_range/0");
    CHECK(reg.find("osc1.unison.detune")->pointer == "/osc1/unison/detune");
    CHECK(reg.find("fx.m2.mix")->pointer == "/fx/master/1/mix");
    // Ranges the plan states.
    CHECK(reg.find("filter1.cutoff")->min == 8.0);
    CHECK(reg.find("filter1.cutoff")->max == 22000.0);
    CHECK(reg.find("global.polyphony")->max == 64.0);
    CHECK(reg.find("global.polyphony")->def == 16.0);
    CHECK(reg.find("osc1.unison.voices")->max == 16.0);
    CHECK(reg.find("global.master_tune")->min == -100.0);
    CHECK(reg.find("global.reference")->min == 400.0);
    CHECK(reg.find("global.reference")->max == 480.0);
    CHECK(reg.find("env1.attack")->max == 30000.0);
    CHECK(reg.find("osc1.coarse")->max == 64.0);
    CHECK(reg.find("filter1.type")->choiceIndex("ladder_lp24") >= 0);
    CHECK(reg.find("osc1.warp_a.mode")->choiceIndex("bend_plus") >= 0);
}

TEST_CASE("registry: modules cover the parameter list without gaps", "[registry]") {
    const auto& reg = Registry::get();
    int next = 0;
    std::set<std::string> ids;
    for (const auto& m : reg.modules()) {
        INFO(m.id);
        CHECK(m.firstParam == next);
        CHECK(m.numParams > 0);
        CHECK(!m.name.empty());
        CHECK(!m.description.empty());
        CHECK(ids.insert(m.id).second);
        for (int i = m.firstParam; i < m.firstParam + m.numParams; ++i)
            CHECK(reg.at(i).module == m.id);
        next += m.numParams;
    }
    CHECK(next == reg.size());
}

TEST_CASE("registry: is append-only against the recorded id list", "[registry]") {
    // tests/data/registry_ids.txt records every id in order. Ids may only be added at the end:
    // when you add parameters, append their ids to that file in the same change.
    const auto& reg = Registry::get();
    std::istringstream in(zedtest::readText(zedtest::dataPath("registry_ids.txt")));
    std::vector<std::string> recorded;
    for (std::string line; std::getline(in, line);)
        if (!line.empty())
            recorded.push_back(line);
    REQUIRE(!recorded.empty());
    REQUIRE(static_cast<int>(recorded.size()) == reg.size());
    for (int i = 0; i < reg.size(); ++i) {
        INFO("position " << i);
        REQUIRE(reg.at(i).id == recorded[static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("registry: effect types fit the generic slot parameters", "[registry]") {
    const auto& reg = Registry::get();
    REQUIRE(reg.effectTypes().size() == 13);
    std::set<std::string> ids;
    for (const auto& e : reg.effectTypes()) {
        INFO(e.id);
        CHECK(ids.insert(e.id).second);
        CHECK(reg.effectIndex(e.id) >= 0);
        CHECK(!e.description.empty());
        CHECK(!e.params.empty());
        CHECK(e.params.size() <= static_cast<std::size_t>(kNumFxSlotParams));
        std::set<std::string> keys = {"type", "on", "mix"};
        for (std::size_t i = 0; i < e.params.size(); ++i) {
            const auto& p = e.params[i];
            INFO(p.id);
            CHECK(p.index == static_cast<int>(i));
            CHECK(keys.insert(p.pointer).second);
            CHECK(p.pointer.find('/') == std::string::npos);
            CHECK(p.def >= p.min);
            CHECK(p.def <= p.max);
            CHECK(p.description.size() > 8);
        }
    }
    CHECK(reg.effectIndex("nothing") == -1);
    for (const char* id : {"drive", "compressor", "eq", "filter", "chorus", "flanger", "phaser", "delay", "reverb",
                           "swarm", "widener", "utility", "limiter"})
        CHECK(reg.effectIndex(id) >= 0);
}

TEST_CASE("registry: modulation sources and tempo divisions", "[registry]") {
    const auto& reg = Registry::get();
    std::set<std::string> ids;
    for (const auto& s : reg.modSources()) {
        CHECK(ids.insert(s.id).second);
        CHECK(reg.isModSource(s.id));
        CHECK(!s.description.empty());
    }
    for (const char* id : {"env1", "env4", "lfo8", "flux2", "macro8", "velocity", "modwheel", "pitchbend", "cc0", "cc74", "cc127"})
        CHECK(reg.isModSource(id));
    for (const char* id : {"", "lfo9", "cc128", "cc", "cc-1", "cc01", "cc1x", "Env1"})
        CHECK_FALSE(reg.isModSource(id));

    CHECK(divisionInBeats("1/4") == 1.0);
    CHECK(divisionInBeats("1/4D") == 1.5);
    CHECK(divisionInBeats("1/8T") == Approx(1.0 / 3.0));
    CHECK(divisionInBeats("8/1") == 32.0);
    CHECK(divisionInBeats("1/128") == 4.0 / 128.0);
    CHECK(divisionInBeats("bogus") == 0.0);
    CHECK(divisionInBeats("1/0") == 0.0);
    std::set<double> lengths;
    for (const auto& d : tempoDivisions()) {
        INFO(d);
        CHECK(divisionInBeats(d) > 0.0);
        CHECK(lengths.insert(divisionInBeats(d)).second); // no two names for one length
    }
    CHECK(*lengths.rbegin() == 32.0);
    CHECK(*lengths.begin() == 4.0 / 128.0);
}

TEST_CASE("registry: generated documents cover every parameter", "[registry][docs]") {
    const auto& reg = Registry::get();
    const std::string md = registryMarkdown();
    for (const auto& m : reg.modules()) {
        if (m.kind == ModuleKind::ModRoute || m.kind == ModuleKind::FxSlot)
            continue; // documented once, as a pattern
        for (int i = m.firstParam; i < m.firstParam + m.numParams; ++i) {
            INFO(reg.at(i).id);
            CHECK(md.find("`" + reg.at(i).id + "`") != std::string::npos);
        }
    }
    CHECK(md.find("`mod.N.amount`") != std::string::npos);
    CHECK(md.find("`fx.<slot>.p16`") != std::string::npos);
    for (const auto& e : reg.effectTypes())
        CHECK(md.find("(`" + e.id + "`)") != std::string::npos);

    const auto doc = nlohmann::json::parse(registryJson());
    REQUIRE(doc["params"].size() == static_cast<std::size_t>(reg.size()));
    CHECK(doc["count"] == reg.size());
    CHECK(doc["effects"].size() == reg.effectTypes().size());
    for (std::size_t i = 0; i < doc["params"].size(); ++i) {
        CHECK(doc["params"][i]["id"] == reg.at(static_cast<int>(i)).id);
        CHECK(doc["params"][i]["host_id"] == reg.at(static_cast<int>(i)).hostId);
        CHECK(!doc["params"][i]["description"].get<std::string>().empty());
    }
}
