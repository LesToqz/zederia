#include "zederia/bench/Soak.h"

#include "zederia/engine/Engine.h"
#include "zederia/params/Registry.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/render/OfflineRenderer.h"
#include "zederia/util/Math.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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
std::vector<Target> automationTargets(const Patch& patch, const std::vector<std::string>& keepStill) {
    const auto& reg = Registry::get();
    std::vector<Target> out;
    for (const ParamDef& d : reg.params()) {
        if (!d.automatable)
            continue;
        if (std::any_of(keepStill.begin(), keepStill.end(), [&](const std::string& p) { return d.id.rfind(p, 0) == 0; }))
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
    // The optional trace (a diagnosis aid; see SoakOptions::tracePath).
    std::FILE* trace = nullptr;
#if defined(_MSC_VER)
    if (!o.tracePath.empty() && fopen_s(&trace, o.tracePath.c_str(), "w") != 0)
        trace = nullptr;
#else
    if (!o.tracePath.empty())
        trace = std::fopen(o.tracePath.c_str(), "w");
#endif
    struct CloseTrace {
        std::FILE*& f;
        ~CloseTrace() {
            if (f != nullptr)
                std::fclose(f);
        }
    } closeTrace{trace};
    std::vector<MidiEvent> madeNotes(2048);
    const int midiOutIndex = reg.indexOf("global.midi_out");
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
        // The engine takes a block's events in time order, as a host gives them.
        for (MidiEvent& e : midi)
            e.offset = std::clamp(e.offset, 0, frames - 1);
        std::stable_sort(midi.begin(), midi.end(), [](const MidiEvent& a, const MidiEvent& b) { return a.offset < b.offset; });
        ProcessArgs args{left.data(), right.data(), frames, midi, transport};
        int made = 0;
        if (trace != nullptr) { // the notes the play features make, when the trace asks for them
            engine.params().set(midiOutIndex, 1.0);
            args.midiOut = madeNotes.data();
            args.midiOutCapacity = static_cast<int>(madeNotes.size());
            args.midiOutCount = &made;
            for (const MidiEvent& e : midi)
                if (e.type == MidiEvent::Type::NoteOn || e.type == MidiEvent::Type::NoteOff ||
                    (e.type == MidiEvent::Type::Controller && e.number == 64))
                    std::fprintf(trace, "%lld in %s %d %.2f\n", static_cast<long long>(position + e.offset),
                                 e.type == MidiEvent::Type::NoteOn ? "on" : e.type == MidiEvent::Type::NoteOff ? "off" : "pedal", e.number, e.value);
        }
        engine.process(args);
        for (int i = 0; trace != nullptr && i < made; ++i) {
            const MidiEvent& e = madeNotes[static_cast<std::size_t>(i)];
            if (e.type == MidiEvent::Type::NoteOn || e.type == MidiEvent::Type::NoteOff)
                std::fprintf(trace, "%lld made %s %d\n", static_cast<long long>(position + e.offset),
                             e.type == MidiEvent::Type::NoteOn ? "on" : "off", e.number);
        }
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
        if (rng.uniform() < 0.5 || !o.switchWhileHeld) {
            for (const int key : held)
                midi.push_back(MidiEvent::noteOff(0, key));
            held.clear();
        }
        engine.loadPatch(patch);
        if (trace != nullptr)
            std::fprintf(trace, "%lld load %s\n", static_cast<long long>(position), current.c_str());
        const auto problems = applyPatchAssets(engine, patch, o.library, fs::path(current).parent_path().string());
        if (!problems.empty()) {
            ++report.loadProblems;
            note(problems.front());
        }
        ++report.presetsLoaded;
        const std::vector<Target> targets = automationTargets(patch, o.keepStill);

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
                if (trace != nullptr) {
                    const std::string& id = reg.at(t.index).id;
                    if (id.rfind("arp.", 0) == 0 || id.rfind("chord.", 0) == 0 || id.rfind("clip.", 0) == 0)
                        std::fprintf(trace, "%lld set %s %g\n", static_cast<long long>(position), id.c_str(), value);
                }
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
        // A release may last as long as the amplitude envelope's release time, which the moves can
        // have set to its 30 s top (and modulation further), stretched up to twice by the per-note
        // envelope variance: allow that, with a margin, before calling the voices stuck.
        const int releaseIndex = reg.indexOf("env1.release"), varianceIndex = reg.indexOf("voice.variance.env");
        const double releaseMs = std::max(engine.params().get(releaseIndex), engine.liveValue(releaseIndex));
        const double variance = std::clamp(engine.params().get(varianceIndex), 0.0, 1.0);
        const double allowed = std::max(o.maxReleaseSeconds, releaseMs * 0.001 * (1.0 + variance) * 1.1 + 2.0);
        const std::int64_t limit = position + static_cast<std::int64_t>(allowed * sr);
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
            // Does it end at all? Wait two more minutes and say when.
            const bool arpRunning = engine.arpStep() >= 0;
            const std::int64_t stuckAt = position;
            double endedAfter = -1.0;
            while (position < stuckAt + static_cast<std::int64_t>(120.0 * sr)) {
                block(randomFrames());
                if (engine.activeVoices() == 0) {
                    endedAfter = static_cast<double>(position - stuckAt) / sr + allowed;
                    break;
                }
            }
            note(endedAfter < 0.0 ? std::string("the voices were still sounding two minutes later") +
                                        (arpRunning ? " (the arpeggiator was still stepping)" : " (the arpeggiator was not stepping)")
                                  : "the voices ended " + std::to_string(static_cast<int>(endedAfter)) + " s after the keys were let go" +
                                        (arpRunning ? " (the arpeggiator was still stepping)" : ""));
            const auto value = [&](const char* id) { return engine.params().get(reg.indexOf(id)); };
            char detail[320];
            std::snprintf(detail, sizeof(detail),
                          " s after every key was let go (release %.0f ms, envelope variance %.0f %%; arpeggiator %s, rate %s, gate %.2f, "
                          "%d steps; chord memory %s, scale lock %s, strum %.0f ms; MPE %s)",
                          releaseMs, variance * 100.0, value("arp.on") >= 0.5 ? "on" : "off",
                          reg.at(reg.indexOf("arp.rate")).format(value("arp.rate")).c_str(), value("arp.gate"),
                          static_cast<int>(value("arp.steps")), value("chord.memory") >= 0.5 ? "on" : "off",
                          value("chord.scale_lock") >= 0.5 ? "on" : "off", value("chord.strum"), value("global.mpe") >= 0.5 ? "on" : "off");
            note(std::to_string(engine.activeVoices()) + " voice(s) still sounding " + std::to_string(static_cast<int>(allowed)) + detail);
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
