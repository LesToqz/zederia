// Ten thousand malformed or fuzzed documents must load without a crash (plan 6.1). Whatever
// loads successfully must also be internally consistent: in range and stable under save/load.
#include "../support/TestUtil.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/util/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace zed;

namespace {

using J = nlohmann::ordered_json;

std::string randomString(Rng& rng) {
    static const char* interesting[] = {"", "poly", "ladder_lp24", "lfo1", "filter1.cutoff", "chorus", "true", "null",
                                        "zederia.patch", "\xC3\xA9\xE2\x99\xAA", "a/b~c", "1/8T", "cc74", "../../x"};
    const auto pick = rng.next() % 20;
    if (pick < std::size(interesting))
        return interesting[pick];
    std::string s;
    const std::size_t length = pick == 19 ? 20000 : rng.next() % 24;
    for (std::size_t i = 0; i < length; ++i)
        s += static_cast<char>(' ' + rng.next() % 94);
    return s;
}

J randomValue(Rng& rng, int depth = 0) {
    switch (rng.next() % (depth > 2 ? 8 : 11)) {
    case 0: return nullptr;
    case 1: return rng.next() % 2 == 0;
    case 2: return static_cast<std::int64_t>(rng.next() % 2001) - 1000;
    case 3: return (rng.uniform() - 0.5) * 4.0;
    case 4: return rng.next() % 2 ? 1e308 : -1e308;
    case 5: return rng.next() % 2 ? std::int64_t{9007199254740993} : std::int64_t{-9223372036854775807};
    case 6: return randomString(rng);
    case 7: return rng.uniform() * 1e-320; // a denormal
    case 8: {
        J a = J::array();
        const std::size_t n = rng.next() % 5 == 0 ? 300 : rng.next() % 6;
        for (std::size_t i = 0; i < n; ++i)
            a.push_back(randomValue(rng, depth + 1));
        return a;
    }
    default: {
        J o = J::object();
        const std::size_t n = rng.next() % 6;
        for (std::size_t i = 0; i < n; ++i)
            o[randomString(rng)] = randomValue(rng, depth + 1);
        return o;
    }
    }
}

// Replaces, renames or removes something at a random place in the tree.
void mutateTree(J& node, Rng& rng) {
    if (node.is_object() && !node.empty()) {
        auto it = node.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(rng.next() % node.size()));
        const std::string key = it.key();
        switch (rng.next() % 6) {
        case 0: node[key] = randomValue(rng); return;
        case 1: node.erase(key); return;
        case 2: node[randomString(rng)] = randomValue(rng); return;
        case 3: {
            J moved = node[key];
            node.erase(key);
            node[key + "x"] = std::move(moved);
            return;
        }
        default: mutateTree(node[key], rng); return;
        }
    }
    if (node.is_array() && !node.empty() && rng.next() % 3 != 0) {
        const std::size_t i = rng.next() % node.size();
        if (rng.next() % 4 == 0)
            node.push_back(node[i]);
        else
            mutateTree(node[i], rng);
        return;
    }
    node = randomValue(rng);
}

void mutateBytes(std::string& text, Rng& rng) {
    static const char punctuation[] = "{}[]\",:\\0123456789-+.eE \n\ttruefalsenull";
    const int edits = 1 + static_cast<int>(rng.next() % 4);
    for (int e = 0; e < edits && !text.empty(); ++e) {
        const std::size_t at = rng.next() % text.size();
        switch (rng.next() % 6) {
        case 0: text[at] = static_cast<char>(rng.next() % 256); break;
        case 1: text.erase(at, 1 + rng.next() % 20); break;
        case 2: text.insert(at, 1, punctuation[rng.next() % (sizeof punctuation - 1)]); break;
        case 3: text.resize(at); break;
        case 4: text.insert(at, text.substr(at, rng.next() % 40)); break;
        default: text[at] = punctuation[rng.next() % (sizeof punctuation - 1)]; break;
        }
    }
}

struct Tally {
    int loaded = 0, refused = 0, problems = 0;
    std::string firstProblem;
    void problem(const std::string& what, const std::string& text) {
        if (problems++ == 0)
            firstProblem = what + "\n--- document ---\n" + text.substr(0, 2000);
    }
};

void check(const std::string& text, Tally& tally) {
    const LoadResult r = loadPatch(text);
    for (const auto& i : r.issues)
        if (i.code.empty() || i.message.empty() || i.fix.empty() || (!i.path.empty() && i.path[0] != '/'))
            tally.problem("an issue lacks a code, message, fix or valid path", text);
    if (!r.ok()) {
        ++tally.refused;
        if (r.issues.empty())
            tally.problem("refused without saying why", text);
        return;
    }
    ++tally.loaded;
    const auto& reg = Registry::get();
    for (int i = 0; i < reg.size(); ++i) {
        const ParamDef& d = r.patch.effectiveDef(i);
        const double v = r.patch.get(i);
        if (!(v >= d.min && v <= d.max) || (d.stepped() && v != std::round(v)))
            tally.problem("value out of range after load: " + reg.at(i).id, text);
    }
    if (r.patch.routes.size() > static_cast<std::size_t>(kNumModRoutes))
        tally.problem("too many routes after load", text);
    const std::string saved = savePatch(r.patch);
    const LoadResult again = loadPatch(saved);
    if (!again.ok() || !again.issues.empty())
        tally.problem("a saved patch did not load cleanly", text);
    else if (!(again.patch == r.patch))
        tally.problem("a saved patch loaded differently", text);
    else if (savePatch(again.patch) != saved)
        tally.problem("save, load, save was not identical", text);
}

} // namespace

TEST_CASE("patch fuzz: ten thousand damaged documents load without a crash", "[patch][fuzz]") {
    const std::string example = zedtest::readText(zedtest::dataPath("patches/glass_pluck.zpreset"));
    REQUIRE(loadPatch(example).ok());
    Patch busy = loadPatch(example).patch;
    busy.addFxSlot(1, Registry::get().effectIndex("delay"));
    busy.addFxSlot(1, Registry::get().effectIndex("compressor"));
    const std::string full = savePatch(busy); // every field present
    const J exampleTree = J::parse(example);
    const J fullTree = J::parse(full);

    Rng rng(20261110);
    Tally tally;
    int count = 0;

    for (int i = 0; i < 4500; ++i, ++count) { // structure kept, content damaged
        J tree = i % 9 == 0 ? fullTree : exampleTree;
        const int edits = 1 + static_cast<int>(rng.next() % 3);
        for (int e = 0; e < edits; ++e)
            mutateTree(tree, rng);
        check(tree.dump(-1, ' ', false, J::error_handler_t::replace), tally);
    }
    for (int i = 0; i < 4000; ++i, ++count) { // text damaged
        std::string text = i % 16 == 0 ? full : example;
        mutateBytes(text, rng);
        check(text, tally);
    }
    for (int i = 0; i < 1000; ++i, ++count) { // not a patch at all
        std::string text;
        if (i % 2 == 0) {
            const std::size_t length = rng.next() % 400;
            for (std::size_t b = 0; b < length; ++b)
                text += static_cast<char>(rng.next() % 256);
        } else {
            text = randomValue(rng).dump(-1, ' ', false, J::error_handler_t::replace);
        }
        check(text, tally);
    }
    // Hand-picked hostile shapes.
    const std::string head = "{\"format\":\"zederia.patch\",\"version\":1,";
    std::vector<std::string> hostile = {
        head + "\"osc1\":" + std::string(20000, '[') + std::string(20000, ']') + "}",
        head + "\"mod\":" + std::string(5000, '[') + "{}" + std::string(5000, ']') + "}",
        std::string(100000, '{'),
        head + "\"meta\":{\"name\":\"" + std::string(300000, 'n') + "\"}}",
        head + "\"meta\":{\"tags\":[" + [] { std::string s; for (int i = 0; i < 5000; ++i) s += "\"t\","; return s; }() + "\"t\"]}}",
        head + "\"filter1\":{\"cutoff_hz\":1e400}}",
        head + "\"filter1\":{\"cutoff_hz\":-0.0,\"resonance\":1e-320}}",
        head + "\"global\":{\"polyphony\":18446744073709551615}}",
        head + "\"osc1\":{\"on\":true,\"on\":false,\"on\":7}}",
        "\xEF\xBB\xBF" + head + "\"osc1\":{}}",
        head + "\"meta\":{\"name\":\"\\ud800\"}}",
        head + "\"meta\":{\"name\":\"\xFF\xFE\"}}",
        std::string("{\"format\":\"zederia.patch\",\"version\":1}\0trailing", 47),
        head + "\"version\":99}",
    };
    while (count < 10000) {
        check(hostile[static_cast<std::size_t>(count) % hostile.size()], tally);
        ++count;
    }

    INFO(tally.firstProblem);
    CHECK(tally.problems == 0);
    CHECK(count == 10000);
    // The corpus must exercise both outcomes, or it proves little.
    INFO("loaded " << tally.loaded << ", refused " << tally.refused);
    CHECK(tally.loaded > 1000);
    CHECK(tally.refused > 1000);
}
