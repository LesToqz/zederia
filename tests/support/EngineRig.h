// A small harness for tests that drive the engine block by block: it keeps everything the engine
// produced, moves the transport along, and reads what the engine reports about itself.
#pragma once

#include "TestUtil.h"
#include "zederia/table/Wavetable.h"
#include "zederia/util/Math.h"

#include <memory>
#include <string>
#include <vector>

namespace zedtest {

struct EngineRig {
    zed::Engine engine;
    std::vector<float> left, right;
    zed::Transport transport; // the song position advances by itself while `playing` is set
    double sampleRate;

    explicit EngineRig(const zed::Patch& patch = zed::Patch(), double rate = 48000.0) : sampleRate(rate) {
        engine.prepare(rate);
        engine.loadPatch(patch);
    }

    // One block of `frames` samples.
    void run(int frames, const std::vector<zed::MidiEvent>& events = {}) {
        const std::size_t start = left.size();
        left.resize(start + static_cast<std::size_t>(frames));
        right.resize(start + static_cast<std::size_t>(frames));
        engine.process({left.data() + start, right.data() + start, frames, events, transport});
        if (transport.playing)
            transport.ppqPosition += frames * transport.bpm / (60.0 * sampleRate);
    }
    // `frames` samples in blocks of `block`.
    void runBlocks(int frames, int block = 16) {
        for (int done = 0; done < frames; done += block)
            run(std::min(block, frames - done));
    }
    // Runs in control-interval steps until the recording is `milliseconds` long (to the nearest
    // control tick).
    void runUntil(double milliseconds) {
        const auto target = static_cast<std::size_t>(milliseconds * sampleRate / 1000.0 / 16.0 + 0.5) * 16;
        while (left.size() < target)
            run(16);
    }
    // Runs until what the engine reports (sources, live values) describes the moment
    // `milliseconds` after the start. The engine is always one control interval ahead of the
    // audio: at each tick it works out where everything will be at the next one, and that is
    // what it reports.
    void runTo(double milliseconds) { runUntil(milliseconds - 16000.0 / sampleRate); }

    float source(const char* id) const { return engine.sourceValue(zed::Engine::sourceIndex(id)); }
    double live(const char* id) const { return engine.liveValue(idx(id)); }
    void set(const char* id, double value) { engine.params().set(idx(id), value); }
};

// What a rig recorded, for tests that render and compare.
struct Stereo {
    std::vector<float> left, right;
};

inline int choiceOf(const char* id, const char* token) { return zed::Registry::get().find(id)->choiceIndex(token); }

// Appends a route and sets its amount.
inline zed::ModRoute& addRoute(zed::Patch& patch, const char* source, const char* dest, double amount,
                               zed::Polarity polarity = zed::Polarity::Uni, zed::ModCurve curve = zed::ModCurve::Linear) {
    zed::ModRoute route;
    route.source = source;
    route.dest = dest;
    route.polarity = polarity;
    route.curve = curve;
    patch.routes.push_back(route);
    patch.set(zed::Registry::get().modAmount(static_cast<int>(patch.routes.size()) - 1), amount);
    return patch.routes.back();
}

// A straight line from (0, from) to (1, to): a ramp LFO, or a linear curve.
inline zed::Shape lineShape(double from, double to) {
    zed::Shape shape;
    shape.points.push_back({0.0, from, 0.0});
    shape.points.push_back({1.0, to, 0.0});
    return shape;
}

// A one-frame wavetable holding a sine: an oscillator that plays a pure tone.
inline std::shared_ptr<const zed::TableData> sineTableData() {
    zed::Wavetable table;
    zed::Frame frame;
    for (int n = 0; n < zed::kFrameSize; ++n)
        frame[static_cast<std::size_t>(n)] = static_cast<float>(std::sin(2.0 * zed::kPi * n / zed::kFrameSize));
    table.frames.push_back(frame);
    return zed::TableData::build(table);
}

// A flat envelope, full level, fixed phase, and no source unless a test switches one on.
inline zed::Patch flatPatch() {
    zed::Patch p;
    p.set("osc1.on", 0);
    p.set("global.master", 0);
    p.set("env1.attack", 0);
    p.set("env1.sustain", 1);
    p.set("env1.release", 5);
    for (const char* osc : {"osc1", "osc2", "osc3"}) {
        p.set(std::string(osc) + ".phase_random", 0);
        p.set(std::string(osc) + ".level", 0);
    }
    p.set("sub.level", 0);
    p.set("texture.level", 0);
    p.set("voice.velocity_sens", 0);
    return p;
}

inline double maxDifference(const std::vector<float>& a, const std::vector<float>& b) {
    double worst = a.size() == b.size() ? 0.0 : 1.0e9;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
        worst = std::max(worst, std::abs(static_cast<double>(a[i]) - b[i]));
    return worst;
}

} // namespace zedtest
