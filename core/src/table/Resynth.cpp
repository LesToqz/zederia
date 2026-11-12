#include "zederia/table/Resynth.h"

#include "zederia/render/OfflineRenderer.h"
#include "zederia/util/Math.h"

#include <algorithm>
#include <cmath>

namespace zed {

Frame renderWarpedCycle(const TableData& table, double position, const WarpSetup& warp) {
    const auto usable = [](osc::WarpMode mode) { return osc::warpUsesModulator(mode) ? osc::WarpMode::Off : mode; };
    std::array<float, osc::kCurvePoints + 2> curveA{}, curveB{};
    osc::fillWarpCurve(warp.curveA, curveA.data());
    osc::fillWarpCurve(warp.curveB, curveB.data());

    osc::Lanes lanes{};
    lanes.count = 1;
    lanes.inc[0] = 1.0f / kFrameSize; // one cycle in exactly 2048 samples
    lanes.fpos[0] = static_cast<float>(std::clamp(position, 0.0, 1.0) * (table.frames() - 1));
    if (table.stepped())
        lanes.fpos[0] = std::floor(lanes.fpos[0] + 0.5f);
    lanes.gL[0] = 1.0f;
    lanes.dA[0] = osc::warpDerive(usable(warp.modeA), static_cast<float>(warp.amountA));
    lanes.dB[0] = osc::warpDerive(usable(warp.modeB), static_cast<float>(warp.amountB));
    const TableData::Level& level = table.level(0); // every harmonic, as low notes play it
    lanes.tabOff[0] = lanes.tabOff2[0] = static_cast<float>(level.offset);
    lanes.tabStride[0] = lanes.tabStride2[0] = static_cast<float>(level.stride);
    lanes.tabSize[0] = lanes.tabSize2[0] = static_cast<float>(level.size);

    osc::Block block;
    block.table = table.data();
    block.maxFrame = static_cast<float>(table.frames() - 1);
    block.modeA = usable(warp.modeA);
    block.modeB = usable(warp.modeB);
    block.curveA = curveA.data();
    block.curveB = curveB.data();
    Frame out;
    for (int done = 0; done < kFrameSize; done += osc::kMaxRun) {
        block.samples = std::min(osc::kMaxRun, kFrameSize - done);
        block.outL = out.data() + done;
        osc::renderScalar(lanes, block);
    }
    return out;
}

Wavetable bakeWarp(const Wavetable& table, double position, osc::WarpMode mode, int frames, std::span<const float> curve) {
    Wavetable out;
    out.name = table.name;
    frames = std::clamp(frames, 1, kMaxFrames);
    const auto data = TableData::build(table);
    WarpSetup warp;
    warp.modeA = mode;
    warp.curveA = curve;
    for (int f = 0; f < frames; ++f) {
        warp.amountA = frames > 1 ? static_cast<double>(f) / (frames - 1) : 0.0;
        out.frames.push_back(renderWarpedCycle(*data, position, warp));
    }
    return out;
}

bool bakePatch(const Patch& patch, const BakePatchOptions& options, Wavetable& out, std::string& error) {
    out = Wavetable{};
    if (options.bars <= 0.0 || options.bars > 64.0 || options.bpm < 20.0 || options.bpm > 999.0 || options.note < 0 ||
        options.note > 127) {
        error = "bake patch needs 0 to 64 bars, a tempo from 20 to 999 and a note from 0 to 127";
        return false;
    }
    const double reference = patch.get("global.reference") * std::exp2(patch.get("global.master_tune") / 1200.0);
    const double frequency = noteToHz(options.note, reference);
    const double seconds = options.bars * 4.0 * 60.0 / options.bpm;
    RenderSettings settings;
    settings.sampleRate = frequency * kFrameSize; // one cycle of the note is one frame
    settings.seed = options.seed;
    settings.bpm = options.bpm;
    settings.tailSeconds = 0.0;
    settings.tables = options.tables;
    settings.texture = options.texture;
    const NoteSpec notes[] = {{options.note, 0.0, seconds, 1.0}};
    const RenderResult audio = renderOffline(patch, sequenceFromNotes(notes, settings.sampleRate), settings);
    const int cycles = static_cast<int>(audio.left.size() / kFrameSize);
    if (cycles < 1) {
        error = "the render is shorter than one cycle";
        return false;
    }
    const int frames = std::clamp(std::min(options.frames, cycles), 1, kMaxFrames);
    for (int f = 0; f < frames; ++f) {
        const int cycle = frames > 1 ? static_cast<int>(std::llround(static_cast<double>(f) * (cycles - 1) / (frames - 1))) : 0;
        Frame frame;
        for (int n = 0; n < kFrameSize; ++n) {
            const std::size_t at = static_cast<std::size_t>(cycle) * kFrameSize + static_cast<std::size_t>(n);
            frame[static_cast<std::size_t>(n)] = 0.5f * (audio.left[at] + audio.right[at]);
        }
        out.frames.push_back(frame);
    }
    out.name = patch.meta.name;
    return true;
}

Frame shapeToFrame(std::span<const float> values, bool unipolar) {
    Frame out{};
    if (values.empty())
        return out;
    const auto last = static_cast<double>(values.size() - 1);
    for (int n = 0; n < kFrameSize; ++n) {
        const double at = static_cast<double>(n) / kFrameSize * last;
        const auto i = static_cast<std::size_t>(at);
        const std::size_t next = std::min(i + 1, values.size() - 1);
        const double v = values[i] + (values[next] - values[i]) * (at - static_cast<double>(i));
        out[static_cast<std::size_t>(n)] = static_cast<float>(unipolar ? 2.0 * v - 1.0 : v);
    }
    return out;
}

} // namespace zed
