#include "zederia/bench/Soak.h"

#include "zederia/engine/Engine.h"
#include "zederia/params/Registry.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/render/OfflineRenderer.h"
#include "zederia/util/Math.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

namespace zed {
namespace {

namespace fs = std::filesystem;

struct Target {
    int index = 0;
    double min = 0.0, max = 1.0;
    bool stepped = false;
};

// Every parameter a host could automate, in the units the store holds, except plain gain stages
// (levels in dB): turned fully up those make a loud sound legitimately, which would hide a real
// runaway behind them. Effect slots use their loaded effect's ranges.
std::vector<Target> automationTargets(const Patch& patch) {
    const auto& reg = Registry::get();
    std::vector<Target> out;
    for (const ParamDef& d : reg.params()) {
        if (!d.automatable)
            continue;
        int lane = 0, slot = 0, k = 0;
        if (reg.fxSlotParamAt(d.index, lane, slot, k)) {
            const int type = slot < patch.fxSlotCount(lane) ? patch.fxSlotType(lane, slot) : -1;
            if (type < 0)
                continue;
            const auto& effect = reg.effectTypes()[static_cast<std::size_t>(type)];
            if (k >= static_cast<int>(effect.params.size()))
                continue;
            const ParamDef& e = effect.params[static_cast<std::size_t>(k)];
            if (e.unit != Unit::Db)
                out.push_back({d.index, e.min, e.max, e.stepped()});
            continue;
        }
        if (d.unit != Unit::Db)
            out.push_back({d.index, d.min, d.max, d.stepped()});
    }
    return out;
}

double seconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

SoakReport runSoak(const SoakOptions& o) {
    SoakReport report;
    const auto started = std::chrono::steady_clock::now();
    const auto& reg = Registry::get();
    Rng rng(o.seed * 0x9E3779B97F4A7C15ull + 99u);
    const double sr = std::clamp(o.sampleRate, 8000.0, 384000.0);
    const int maxBlock = std::clamp(o.maxBlock, 1, 8192);
    const auto total = static_cast<std::int64_t>(std::max(0.0, o.seconds) * sr);

    Engine engine;
    engine.setSeed(o.seed);
    engine.prepare(sr);
    engine.setOffline(!o.realtimeQuality);

    std::vector<float> left(static_cast<std::size_t>(maxBlock)), right(static_cast<std::size_t>(maxBlock));
    std::vector<MidiEvent> midi;
    midi.reserve(64);
    std::vector<int> held;
    Transport transport;
    transport.playing = true;
    std::int64_t position = 0;
    std::string current = "init";
    const auto note = [&](const std::string& what) {
        if (report.events.size() < 100)
            report.events.push_back({static_cast<double>(position) / sr, current, what});
    };

    // One block of `frames` samples with the events in `midi`; checks the output.
    const auto block = [&](int frames) {
        transport.ppqPosition = static_cast<double>(position) / sr * transport.bpm / 60.0;
        engine.process({left.data(), right.data(), frames, midi, transport});
        midi.clear();
        float peak = 0.0f;
        bool finite = true;
        for (int i = 0; i < frames; ++i) {
            const float l = left[static_cast<std::size_t>(i)], r = right[static_cast<std::size_t>(i)];
            finite = finite && std::isfinite(l) && std::isfinite(r);
            peak = std::max({peak, std::abs(l), std::abs(r)});
        }
        if (!finite) {
            ++report.nonFiniteBlocks;
            note("the output held a value that is not a number");
        } else if (peak > 0.0f) {
            const double db = 20.0 * std::log10(static_cast<double>(peak));
            report.peakDbfs = std::max(report.peakDbfs, db);
            if (db > o.runawayDbfs) {
                if (report.runawayBlocks == 0 || report.events.size() < 100)
                    note("runaway level: " + std::to_string(static_cast<int>(db)) + " dBFS");
                ++report.runawayBlocks;
            }
        }
        position += frames;
        if (position % static_cast<std::int64_t>(sr / 4) < frames)
            engine.collectGarbage(); // what the plug-in's message thread does: retire replaced tables and effects
    };
    const auto randomFrames = [&] { return 1 + static_cast<int>(rng.uniform() * maxBlock); };

    std::vector<std::size_t> order(o.presets.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::size_t step = 0;
    int pass = 0;
    while (position < total) {
        // The next preset: the list once through in a shuffled order, then again in another.
        if (!order.empty() && step == order.size()) {
            step = 0;
            ++pass;
        }
        if (!order.empty() && step == 0)
            for (std::size_t i = order.size(); i > 1; --i)
                std::swap(order[i - 1], order[static_cast<std::size_t>(rng.uniform() * static_cast<double>(i))]);
        Patch patch;
        current = order.empty() ? std::string("init") : o.presets[order[step]];
        if (o.progress) {
            SoakProgress p;
            p.audioSeconds = static_cast<double>(position) / sr;
            p.wallSeconds = seconds(started);
            p.pass = pass;
            p.presetIndex = static_cast<int>(step);
            p.preset = current;
            p.passStart = step == 0;
            if (!o.progress(p))
                break;
        }
        if (!order.empty()) {
            std::vector<std::uint8_t> bytes;
            std::string error;
            LoadResult loaded;
            if (readFile(current, bytes, error))
                loaded = loadPatch(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            if (!loaded.ok()) {
                ++report.loadProblems;
                note("the preset did not load" + (error.empty() ? std::string() : ": " + error));
            } else {
                patch = std::move(loaded.patch);
            }
        }
        ++step;
        // Half the time the new preset arrives while keys are still held, as it can in a host.
        if (rng.uniform() < 0.5) {
            for (const int key : held)
                midi.push_back(MidiEvent::noteOff(0, key));
            held.clear();
        }
        engine.loadPatch(patch);
        const auto problems = applyPatchAssets(engine, patch, o.library, fs::path(current).parent_path().string());
        if (!problems.empty()) {
            ++report.loadProblems;
            note(problems.front());
        }
        ++report.presetsLoaded;
        const std::vector<Target> targets = automationTargets(patch);

        // Play: random notes, controllers and parameter moves.
        const std::int64_t end = std::min(total, position + static_cast<std::int64_t>(std::max(0.5, o.presetSeconds) * sr));
        while (position < end) {
            const int frames = static_cast<int>(std::min<std::int64_t>(randomFrames(), end - position));
            const double span = frames / sr;
            if (rng.uniform() < o.notesPerSecond * span) {
                if (held.size() >= 10 || (!held.empty() && rng.uniform() < 0.45)) {
                    const std::size_t which = static_cast<std::size_t>(rng.uniform() * static_cast<double>(held.size()));
                    midi.push_back(MidiEvent::noteOff(0, held[which], static_cast<float>(rng.uniform())));
                    held.erase(held.begin() + static_cast<std::ptrdiff_t>(which));
                } else {
                    const int key = 24 + static_cast<int>(rng.uniform() * 72.0);
                    if (std::find(held.begin(), held.end(), key) == held.end()) {
                        midi.push_back(MidiEvent::noteOn(static_cast<int>(rng.uniform() * frames), key, 0.05f + 0.95f * static_cast<float>(rng.uniform())));
                        held.push_back(key);
                        ++report.notes;
                    }
                }
            }
            if (rng.uniform() < 3.0 * span) { // controllers: bend, wheel, brightness, aftertouch, now and then the pedal
                const double u = rng.uniform();
                if (u < 0.3)
                    midi.push_back(MidiEvent::pitchBend(0, static_cast<float>(rng.uniform() * 2.0 - 1.0)));
                else if (u < 0.55)
                    midi.push_back(MidiEvent::controller(0, 1, static_cast<float>(rng.uniform())));
                else if (u < 0.75)
                    midi.push_back(MidiEvent::controller(0, 74, static_cast<float>(rng.uniform())));
                else if (u < 0.92)
                    midi.push_back({0, MidiEvent::Type::ChannelPressure, 0, 0, static_cast<float>(rng.uniform())});
                else
                    midi.push_back(MidiEvent::controller(0, 64, rng.uniform() < 0.5 ? 1.0f : 0.0f));
            }
            for (double moves = o.movesPerSecond * span; !targets.empty() && rng.uniform() < moves; moves -= 1.0) {
                const Target& t = targets[static_cast<std::size_t>(rng.uniform() * static_cast<double>(targets.size()))];
                double value = t.min + rng.uniform() * (t.max - t.min);
                if (t.stepped)
                    value = std::round(value);
                engine.params().set(t.index, value);
                ++report.moves;
            }
            block(frames);
        }

        // Let go of everything, switch off what keeps notes going by design (a latched
        // arpeggiator, a clip on the transport, the pedal), and wait for the voices to end.
        for (const int key : held)
            midi.push_back(MidiEvent::noteOff(0, key));
        held.clear();
        midi.push_back(MidiEvent::controller(0, 64, 0.0f));
        engine.params().set(reg.indexOf("arp.latch"), 0.0);
        engine.params().set(reg.indexOf("clip.on"), 0.0);
        engine.params().set(reg.indexOf("clip.latch"), 0.0);
        ++report.releaseChecks;
        const std::int64_t limit = position + static_cast<std::int64_t>(o.maxReleaseSeconds * sr);
        bool ended = false;
        while (position < limit) {
            block(randomFrames());
            if (engine.activeVoices() == 0) {
                ended = true;
                break;
            }
        }
        if (!ended) {
            ++report.stuckReleases;
            note(std::to_string(engine.activeVoices()) + " voice(s) still sounding " + std::to_string(static_cast<int>(o.maxReleaseSeconds)) +
                 " s after every key was let go");
        }
    }
    report.passes = order.empty() ? 0 : pass + 1;
    report.outputRecoveries = engine.outputRecoveries();
    if (report.outputRecoveries > 0)
        note("the engine muted non-finite output " + std::to_string(report.outputRecoveries) + " time(s)");
    report.audioSeconds = static_cast<double>(position) / sr;
    report.wallSeconds = seconds(started);
    return report;
}

} // namespace zed
