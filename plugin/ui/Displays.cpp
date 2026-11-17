#include "Displays.h"

#include "zederia/dsp/Fft.h"
#include "zederia/table/Resynth.h"
#include "zederia/util/Math.h"

#include <cmath>

namespace zui {

namespace {

const zed::Registry& reg() { return zed::Registry::get(); }
int idx(const juce::String& id) { return reg().indexOf(id.toStdString()); }

std::uint64_t mix(std::uint64_t h, double v) { return zed::fnv1a(&v, sizeof v, h); }
std::uint64_t mix(std::uint64_t h, const void* p) { return zed::fnv1a(&p, sizeof p, h); }

juce::Image softwareImage(int w, int h) {
    return juce::Image(juce::Image::ARGB, juce::jmax(1, w), juce::jmax(1, h), true, juce::SoftwareImageType());
}

float physicalScale(juce::Graphics& g) { return juce::jmax(1.0f, g.getInternalContext().getPhysicalPixelScaleFactor()); }

// One cycle of an oscillator as it sounds now (table, live position, warps).
bool oscCycle(Context& ctx, int osc, std::vector<float>& out, std::uint64_t& key) {
    const juce::String m = "osc" + juce::String(osc + 1) + ".";
    auto& engine = ctx.processor.engine();
    const auto table = engine.table(osc);
    const bool classic = ctx.processor.plainValue(idx(m + "engine")) >= 0.5;
    const double position = engine.liveValue(idx(m + "position"));
    const int modeA = static_cast<int>(ctx.processor.plainValue(idx(m + "warp_a.mode")));
    const int modeB = static_cast<int>(ctx.processor.plainValue(idx(m + "warp_b.mode")));
    const double amountA = engine.liveValue(idx(m + "warp_a.amount"));
    const double amountB = engine.liveValue(idx(m + "warp_b.amount"));
    const double shape = engine.liveValue(idx(m + "classic.shape"));
    const double width = engine.liveValue(idx(m + "classic.pulse_width"));
    std::uint64_t h = mix(0xcbf29ce484222325ull, static_cast<const void*>(table.get()));
    for (const double v : {std::round(position * 1024.0), static_cast<double>(modeA), static_cast<double>(modeB), std::round(amountA * 512.0),
                           std::round(amountB * 512.0), classic ? 1.0 : 0.0, std::round(shape * 256.0), std::round(width * 256.0)})
        h = mix(h, v);
    if (h == key && !out.empty())
        return false;
    key = h;
    out.assign(128, 0.0f);
    if (classic) { // saw, square, triangle, sine: a drawing of the morph, not the band-limited audio
        for (int i = 0; i < 128; ++i) {
            const double t = i / 128.0;
            const double saw = 1.0 - 2.0 * t;
            const double square = t < width ? 1.0 : -1.0;
            const double tri = t < 0.5 ? -1.0 + 4.0 * t : 3.0 - 4.0 * t;
            const double sine = std::sin(2.0 * zed::kPi * t);
            const double s = shape * 3.0;
            const double a = s < 1.0 ? saw + (square - saw) * s : s < 2.0 ? square + (tri - square) * (s - 1.0) : tri + (sine - tri) * (s - 2.0);
            out[static_cast<std::size_t>(i)] = static_cast<float>(a);
        }
        return true;
    }
    if (table == nullptr)
        return true;
    zed::WarpSetup warp;
    const auto usable = [](int m2) { return m2 >= 0 && m2 < zed::osc::kNumWarpModes && !zed::osc::warpUsesModulator(static_cast<zed::osc::WarpMode>(m2)); };
    if (usable(modeA)) { warp.modeA = static_cast<zed::osc::WarpMode>(modeA); warp.amountA = amountA; }
    if (usable(modeB)) { warp.modeB = static_cast<zed::osc::WarpMode>(modeB); warp.amountB = amountB; }
    const zed::Frame frame = zed::renderWarpedCycle(*table, juce::jlimit(0.0, 1.0, position), warp);
    for (int i = 0; i < 128; ++i)
        out[static_cast<std::size_t>(i)] = frame[static_cast<std::size_t>(i * 16)];
    return true;
}

void drawCycleLine(juce::Graphics& g, const std::vector<float>& cycle, juce::Rectangle<float> area, juce::Colour colour, float width) {
    if (cycle.empty())
        return;
    float peak = 0.0f;
    for (const float s : cycle)
        peak = std::max(peak, std::abs(s));
    const float scale = peak > 1e-4f ? 1.0f / std::max(peak, 1.0f) : 1.0f;
    juce::Path p;
    for (std::size_t i = 0; i <= cycle.size(); ++i) {
        const float v = cycle[i % cycle.size()] * scale;
        const float x = area.getX() + area.getWidth() * static_cast<float>(i) / static_cast<float>(cycle.size());
        const float y = area.getCentreY() - v * area.getHeight() * 0.46f;
        if (i == 0)
            p.startNewSubPath(x, y);
        else
            p.lineTo(x, y);
    }
    strokeGlow(g, p, colour, width);
}

} // namespace

juce::String formatHz(double hz) { return hz >= 1000.0 ? juce::String(hz / 1000.0, hz >= 10000.0 ? 0 : 1) + "k" : juce::String(juce::roundToInt(hz)); }

// --- WaveMini ----------------------------------------------------------------------------------------

WaveMini::WaveMini(Context& ctx, int source) : ctx_(ctx), source_(source) {
    setInterceptsMouseClicks(false, false);
    setAccessible(false);
}

bool WaveMini::rebuild() {
    if (source_ < 3)
        return oscCycle(ctx_, source_, cycle_, key_);
    if (source_ == 3) {
        const int shape = static_cast<int>(ctx_.processor.plainValue(idx("sub.shape")));
        if (static_cast<std::uint64_t>(shape + 1) == key_ && !cycle_.empty())
            return false;
        key_ = static_cast<std::uint64_t>(shape + 1);
        cycle_.assign(128, 0.0f);
        for (int i = 0; i < 128; ++i) {
            const double t = i / 128.0;
            double v = 0.0;
            switch (shape) {
            case 0: v = std::sin(2.0 * zed::kPi * t); break;
            case 1: v = t < 0.25 ? 4.0 * t : t < 0.75 ? 2.0 - 4.0 * t : 4.0 * t - 4.0; break;
            case 2: v = 1.0 - 2.0 * t; break;
            case 3: v = t < 0.5 ? 1.0 : -1.0; break;
            case 4: v = t < 0.25 ? 1.0 : -1.0; break;
            default: v = std::tanh(4.0 * std::sin(2.0 * zed::kPi * t)) / std::tanh(4.0); break;
            }
            cycle_[static_cast<std::size_t>(i)] = static_cast<float>(v);
        }
        return true;
    }
    const auto sample = ctx_.processor.engine().texture();
    const auto key = reinterpret_cast<std::uint64_t>(sample.get());
    if (key == key_ && !cycle_.empty())
        return false;
    key_ = key;
    cycle_.assign(128, 0.0f);
    if (sample != nullptr && sample->frames > 0) { // the sample's outline: peak per slice, alternating sign
        const int per = std::max(1, sample->frames / 64);
        for (int i = 0; i < 64; ++i) {
            float peak = 0.0f;
            for (int k = 0; k < per && i * per + k < sample->frames; ++k)
                peak = std::max(peak, std::abs(sample->left[static_cast<std::size_t>(i * per + k)]));
            cycle_[static_cast<std::size_t>(2 * i)] = peak;
            cycle_[static_cast<std::size_t>(2 * i + 1)] = -peak;
        }
    }
    return true;
}

bool WaveMini::tick() { return rebuild(); }

void WaveMini::draw(juce::Graphics& g) {
    if (cycle_.empty())
        rebuild();
    const Theme& t = theme();
    const auto area = getLocalBounds().toFloat();
    g.setColour(t.display);
    g.fillRoundedRectangle(area, 3.0f);
    const char* on[] = {"osc1.on", "osc2.on", "osc3.on", "sub.on", "texture.on"};
    const bool active = ctx_.processor.plainValue(idx(on[source_])) >= 0.5;
    g.setColour(t.displayGrid);
    g.fillRect(area.getX(), area.getCentreY(), area.getWidth(), 1.0f);
    drawCycleLine(g, cycle_, area.reduced(3.0f, 2.0f), active ? t.sources : t.textFaint, 1.2f);
}

// --- WavetableView -----------------------------------------------------------------------------------

WavetableView::WavetableView(Context& ctx, int osc) : ctx_(ctx), osc_(osc) {
    setTitle("Wavetable");
    setHelp("Wavetable", "The oscillator's table: frames from front to back, the playing position lit. Click to switch between 3D and the 2D cycle.");
    family = Family::Sources;
}

void WavetableView::setOsc(int osc) {
    osc_ = osc;
    cache_ = {};
    cachedTable_ = nullptr;
    warpKey_ = 0;
    repaint();
}

bool WavetableView::tick() {
    const double position = ctx_.processor.engine().liveValue(idx("osc" + juce::String(osc_ + 1) + ".position"));
    const auto table = ctx_.processor.engine().table(osc_);
    bool dirty = table.get() != cachedTable_;
    if (std::abs(position - position_) > 0.0008) {
        position_ = position;
        dirty = true;
    }
    if (!show3d)
        dirty = oscCycle(ctx_, osc_, cycle_, warpKey_) || dirty;
    return dirty;
}

void WavetableView::mouseDown(const juce::MouseEvent&) {
    show3d = !show3d;
    warpKey_ = 0;
    repaint();
}

void WavetableView::build3d(juce::Rectangle<int> size) {
    const auto table = ctx_.processor.engine().table(osc_);
    cachedTable_ = table.get();
    cachedTheme_ = theme().name;
    cache_ = softwareImage(size.getWidth(), size.getHeight());
    if (table == nullptr)
        return;
    juce::Graphics g(cache_);
    const Theme& t = theme();
    const auto area = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(size.getWidth()), static_cast<float>(size.getHeight()));
    const float depthX = area.getWidth() * 0.26f, depthY = area.getHeight() * 0.34f;
    const float frontW = area.getWidth() - depthX - 16.0f, ampH = area.getHeight() * 0.2f;
    const int frames = table->frames();
    // Positions evenly across the table (the oscillator moves continuously between frames), back to front.
    constexpr int kLines = 40, kPoints = 120;
    g.setColour(t.displayGrid);
    for (const float depth : {0.0f, 1.0f}) { // the floor: the table's first and last position
        const float x0 = area.getX() + 8.0f + depth * depthX, y0 = area.getBottom() - ampH - 10.0f - depth * depthY;
        g.drawLine(x0, y0, x0 + frontW, y0, 1.0f);
    }
    g.drawLine(area.getX() + 8.0f, area.getBottom() - ampH - 10.0f, area.getX() + 8.0f + depthX, area.getBottom() - ampH - 10.0f - depthY, 1.0f);
    g.drawLine(area.getX() + 8.0f + frontW, area.getBottom() - ampH - 10.0f, area.getX() + 8.0f + frontW + depthX, area.getBottom() - ampH - 10.0f - depthY, 1.0f);
    for (int l = kLines - 1; l >= 0; --l) {
        const float depth = static_cast<float>(l) / static_cast<float>(kLines - 1);
        const double framePos = static_cast<double>(depth) * (frames - 1);
        const float x0 = area.getX() + 8.0f + depth * depthX;
        const float y0 = area.getBottom() - ampH - 10.0f - depth * depthY;
        juce::Path p;
        for (int i = 0; i <= kPoints; ++i) {
            const double phase = static_cast<double>(i % kPoints) / kPoints;
            const float v = static_cast<float>(table->read(9, framePos, phase));
            const float x = x0 + frontW * static_cast<float>(i) / kPoints;
            const float y = y0 - juce::jlimit(-1.2f, 1.2f, v) * ampH;
            if (i == 0)
                p.startNewSubPath(x, y);
            else
                p.lineTo(x, y);
        }
        g.setColour(t.sources.withAlpha(0.12f + 0.38f * (1.0f - depth)));
        g.strokePath(p, juce::PathStrokeType(1.0f));
    }
}

void WavetableView::drawCycle(juce::Graphics& g, juce::Rectangle<float> area) {
    if (cycle_.empty())
        oscCycle(ctx_, osc_, cycle_, warpKey_);
    const Theme& t = theme();
    g.setColour(t.displayGrid);
    g.fillRect(area.getX(), area.getCentreY(), area.getWidth(), 1.0f);
    for (int i = 1; i < 4; ++i)
        g.fillRect(area.getX() + area.getWidth() * static_cast<float>(i) / 4.0f, area.getY(), 1.0f, area.getHeight());
    drawCycleLine(g, cycle_, area.reduced(6.0f, 10.0f), t.sources, 1.8f);
}

void WavetableView::draw(juce::Graphics& g) {
    const Theme& t = theme();
    const auto area = getLocalBounds().toFloat();
    drawWell(g, area);
    plot_ = area.reduced(6.0f);
    const juce::String m = "osc" + juce::String(osc_ + 1);
    const bool classic = ctx_.processor.plainValue(idx(m + ".engine")) >= 0.5;
    if (!show3d || classic) {
        drawCycle(g, plot_);
    } else {
        const float scale = physicalScale(g);
        const auto size = juce::Rectangle<int>(juce::roundToInt(plot_.getWidth() * scale), juce::roundToInt(plot_.getHeight() * scale));
        const auto table = ctx_.processor.engine().table(osc_);
        if (!cache_.isValid() || cache_.getBounds() != size || table.get() != cachedTable_ || cachedTheme_ != t.name)
            build3d(size);
        g.drawImage(cache_, plot_);
        // The playing position, lit across the surface.
        if (table != nullptr) {
            const int frames = table->frames();
            const float depthX = plot_.getWidth() * 0.26f, depthY = plot_.getHeight() * 0.34f;
            const float frontW = plot_.getWidth() - depthX - 16.0f / scale, ampH = plot_.getHeight() * 0.2f;
            const float depth = static_cast<float>(juce::jlimit(0.0, 1.0, position_));
            const float x0 = plot_.getX() + 8.0f / scale + depth * depthX;
            const float y0 = plot_.getBottom() - ampH - 10.0f / scale - depth * depthY;
            const double framePos = juce::jlimit(0.0, 1.0, position_) * (frames - 1);
            juce::Path p;
            for (int i = 0; i <= 96; ++i) {
                const float v = static_cast<float>(table->read(9, framePos, static_cast<double>(i % 96) / 96.0));
                const float x = x0 + frontW * static_cast<float>(i) / 96.0f, y = y0 - juce::jlimit(-1.2f, 1.2f, v) * ampH;
                if (i == 0)
                    p.startNewSubPath(x, y);
                else
                    p.lineTo(x, y);
            }
            strokeGlow(g, p, t.displayLine, 1.6f);
        }
    }
    const juce::String caption = (show3d && !classic ? "3D" : "2D") + juce::String("  ") + juce::String(juce::roundToInt(juce::jmax(0.0, position_) * 100.0)) + " %";
    drawText(g, caption, area.reduced(8.0f, 4.0f).removeFromTop(14.0f), kTextSmall, t.textDim, juce::Justification::centredRight);
}

// --- FilterResponseView ------------------------------------------------------------------------------

FilterResponseView::FilterResponseView(Context& ctx, int slot) : ctx_(ctx), slot_(slot) {
    setTitle("Filter response");
    setHelp("Filter response", "The filter's response at the patch's settings (faint) and as it is modulated now (bright); the other filter behind. Click to show phase.");
    family = Family::Filters;
    for (int i = 0; i < 180; ++i)
        hz_.push_back(20.0 * std::pow(1000.0, i / 179.0));
}

void FilterResponseView::mouseDown(const juce::MouseEvent&) {
    showPhase = !showPhase;
    key_ = 0;
    repaint();
}

void FilterResponseView::compute() {
    auto& engine = ctx_.processor.engine();
    for (int s = 0; s < 2; ++s) {
        live_[s].assign(hz_.size(), 0.0f);
        phase_[s].assign(hz_.size(), 0.0f);
        on_[s] = ctx_.processor.plainValue(idx("filter" + juce::String(s + 1) + ".on")) >= 0.5;
        engine.filterResponse(s, hz_, live_[s], phase_[s]);
    }
    // The patch's own settings, without key tracking or modulation.
    const juce::String m = "filter" + juce::String(slot_ + 1) + ".";
    zed::flt::Settings st;
    st.type = static_cast<int>(ctx_.processor.plainValue(idx(m + "type")));
    st.cutoffHz[0] = st.cutoffHz[1] = static_cast<float>(ctx_.processor.plainValue(idx(m + "cutoff")));
    st.resonance = static_cast<float>(ctx_.processor.plainValue(idx(m + "resonance")));
    st.drive = static_cast<float>(ctx_.processor.plainValue(idx(m + "drive")));
    st.character = static_cast<float>(ctx_.processor.plainValue(idx(m + "character")));
    st.mix = static_cast<float>(ctx_.processor.plainValue(idx(m + "mix")));
    st.levelMode = ctx_.processor.plainValue(idx(m + "mix_mode")) >= 0.5;
    const double sr = engine.sampleRate() > 0.0 ? engine.sampleRate() : 48000.0;
    still_.assign(hz_.size(), 0.0f);
    for (std::size_t i = 0; i < hz_.size(); ++i) {
        const auto r = zed::flt::response(st, 0, sr, hz_[i]);
        still_[i] = static_cast<float>(showPhase ? std::arg(r) : 20.0 * std::log10(std::max(std::abs(r), 1e-9)));
    }
}

bool FilterResponseView::tick() {
    auto& engine = ctx_.processor.engine();
    std::uint64_t h = static_cast<std::uint64_t>(slot_ + 1) * 31u + (showPhase ? 7u : 0u);
    for (int s = 0; s < 2; ++s) {
        const zed::FilterView v = engine.filterView(s);
        h = zed::fnv1a(&v.settings, sizeof v.settings, h);
        h = mix(h, v.on ? 1.0 : 0.0);
    }
    for (const char* f : {"type", "cutoff", "resonance", "drive", "character", "mix", "mix_mode", "on"})
        h = mix(h, ctx_.processor.plainValue(idx("filter" + juce::String(slot_ + 1) + "." + f)));
    if (h == key_)
        return false;
    key_ = h;
    compute();
    return true;
}

void FilterResponseView::draw(juce::Graphics& g) {
    if (still_.empty())
        compute();
    const Theme& t = theme();
    const auto area = getLocalBounds().toFloat();
    drawWell(g, area);
    const auto plot = area.reduced(8.0f, 8.0f).withTrimmedBottom(12.0f);
    const float lo = showPhase ? -3.2f : -36.0f, hi = showPhase ? 3.2f : 24.0f;
    const auto xOf = [&](std::size_t i) { return plot.getX() + plot.getWidth() * static_cast<float>(i) / static_cast<float>(hz_.size() - 1); };
    const auto yOf = [&](float v) { return plot.getBottom() - plot.getHeight() * (juce::jlimit(lo, hi, v) - lo) / (hi - lo); };
    g.setColour(t.displayGrid);
    for (const double f : {100.0, 1000.0, 10000.0}) {
        const float x = plot.getX() + plot.getWidth() * static_cast<float>(std::log(f / 20.0) / std::log(1000.0));
        g.fillRect(x, plot.getY(), 1.0f, plot.getHeight());
        drawText(g, formatHz(f), juce::Rectangle<float>(x - 20.0f, plot.getBottom() + 1.0f, 40.0f, 12.0f), kTextSmall, t.textDim, juce::Justification::centred);
    }
    for (const float db : showPhase ? std::vector<float>{-1.5708f, 0.0f, 1.5708f} : std::vector<float>{-24.0f, -12.0f, 0.0f, 12.0f}) {
        g.setColour(db == 0.0f ? t.lineStrong : t.displayGrid);
        g.fillRect(plot.getX(), yOf(db), plot.getWidth(), 1.0f);
    }
    const auto path = [&](const std::vector<float>& v) {
        juce::Path p;
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i == 0)
                p.startNewSubPath(xOf(i), yOf(v[i]));
            else
                p.lineTo(xOf(i), yOf(v[i]));
        }
        return p;
    };
    const int other = 1 - slot_;
    const auto& otherValues = showPhase ? phase_[other] : live_[other];
    if (on_[other] && !otherValues.empty()) {
        g.setColour(t.filters.withAlpha(0.25f));
        g.strokePath(path(otherValues), juce::PathStrokeType(1.0f));
    }
    g.setColour(t.filters.withAlpha(0.4f));
    g.strokePath(path(still_), juce::PathStrokeType(1.0f));
    const auto& mine = showPhase ? phase_[slot_] : live_[slot_];
    if (!mine.empty())
        strokeGlow(g, path(mine), on_[slot_] ? t.filters : t.textFaint, 1.8f);
    const juce::String caption = juce::String("FILTER ") + juce::String(slot_ + 1) + (showPhase ? "  PHASE" : "") + (on_[slot_] ? "" : "  OFF");
    drawText(g, caption, area.reduced(10.0f, 6.0f).removeFromTop(14.0f), kTextSmall, t.textDim, juce::Justification::centredRight, true);
}

// --- EnvelopeView ------------------------------------------------------------------------------------

EnvelopeView::EnvelopeView(Context& ctx, int env) : ctx_(ctx), env_(env) {
    setTitle("Envelope");
    setHelp("Envelope", "Drag the points: across for the times, up and down at the decay point for the sustain level. Hold Shift for fine moves.");
    family = Family::Modulators;
}

int EnvelopeView::param(const char* suffix) const { return idx("env" + juce::String(env_ + 1) + "." + suffix); }

EnvelopeView::Geometry EnvelopeView::geometry() const {
    // Each segment gets width by the square root of its time, so short and long times both read.
    const double times[5] = {values_[0], values_[1], values_[2], values_[3], values_[5]};
    double weights[6];
    double total = 0.0;
    for (int i = 0; i < 4; ++i)
        total += weights[i] = std::sqrt(std::max(0.0, times[i])) + 1.0;
    weights[4] = std::max(8.0, total * 0.25); // sustain: a fixed share
    weights[5] = std::sqrt(std::max(0.0, times[4])) + 1.0;
    total += weights[4] + weights[5];
    Geometry geo;
    const auto plot = getLocalBounds().toFloat().reduced(14.0f, 12.0f);
    float x = plot.getX();
    for (int i = 0; i < 6; ++i) {
        x += plot.getWidth() * static_cast<float>(weights[i] / total);
        geo.x[i] = x;
    }
    geo.totalMs = static_cast<float>(times[0] + times[1] + times[2] + times[3] + times[4]);
    return geo;
}

bool EnvelopeView::tick() {
    bool dirty = false;
    static const char* const names[8] = {"delay", "attack", "hold", "decay", "sustain", "release", "attack_curve", "decay_curve"};
    for (int i = 0; i < 8; ++i) {
        const double v = ctx_.processor.plainValue(param(names[i]));
        if (v != values_[i]) {
            values_[i] = v;
            dirty = true;
        }
    }
    const int source = Context::sourceIndex("env" + juce::String(env_ + 1));
    const float level = source >= 0 ? ctx_.processor.engine().sourceValue(source) : 0.0f;
    if (std::abs(level - level_) > 0.004f) {
        level_ = level;
        dirty = true;
    }
    return dirty;
}

void EnvelopeView::draw(juce::Graphics& g) {
    const Theme& t = theme();
    const auto area = getLocalBounds().toFloat();
    drawWell(g, area);
    const auto plot = area.reduced(14.0f, 12.0f);
    const auto geo = geometry();
    const float sustain = static_cast<float>(values_[4]);
    const float releaseCurve = static_cast<float>(ctx_.processor.plainValue(param("release_curve")));
    const auto yOf = [&](float level) { return plot.getBottom() - plot.getHeight() * level; };
    juce::Path p;
    p.startNewSubPath(plot.getX(), yOf(0.0f));
    p.lineTo(geo.x[0], yOf(0.0f));
    const auto segment = [&](float x0, float x1, float from, float to, double curve) {
        for (int i = 1; i <= 24; ++i) {
            const double f = zed::curveValue(i / 24.0, curve);
            p.lineTo(x0 + (x1 - x0) * static_cast<float>(i) / 24.0f, yOf(from + (to - from) * static_cast<float>(f)));
        }
    };
    segment(geo.x[0], geo.x[1], 0.0f, 1.0f, values_[6]);
    p.lineTo(geo.x[2], yOf(1.0f));
    segment(geo.x[2], geo.x[3], 1.0f, sustain, values_[7]);
    p.lineTo(geo.x[4], yOf(sustain));
    segment(geo.x[4], geo.x[5], sustain, 0.0f, releaseCurve);
    juce::Path fill(p);
    fill.lineTo(geo.x[5], yOf(0.0f));
    fill.closeSubPath();
    g.setColour(t.modulators.withAlpha(0.12f));
    g.fillPath(fill);
    g.setColour(t.displayGrid);
    for (int i = 0; i < 5; ++i)
        g.fillRect(geo.x[i], plot.getY(), 1.0f, plot.getHeight());
    strokeGlow(g, p, t.modulators, 1.8f);
    const juce::Point<float> handles[4] = {{geo.x[1], yOf(1.0f)}, {geo.x[2], yOf(1.0f)}, {geo.x[3], yOf(sustain)}, {geo.x[5], yOf(0.0f)}};
    for (int i = 0; i < 4; ++i) {
        const float r = i == hoverHandle_ || i == dragHandle_ ? 5.0f : 3.8f;
        g.setColour(t.display);
        g.fillEllipse(juce::Rectangle<float>(2 * r, 2 * r).withCentre(handles[i]));
        g.setColour(i == dragHandle_ ? t.focus : t.modulators);
        g.drawEllipse(juce::Rectangle<float>(2 * r, 2 * r).withCentre(handles[i]), 1.4f);
    }
    if (level_ > 0.001f) {
        g.setColour(t.displayLine.withAlpha(0.5f));
        g.fillRect(plot.getX(), yOf(level_), plot.getWidth(), 1.0f);
    }
    static const char* const labels[] = {"A", "H", "D", "S", "R"};
    const float xs[] = {(geo.x[0] + geo.x[1]) * 0.5f, (geo.x[1] + geo.x[2]) * 0.5f, (geo.x[2] + geo.x[3]) * 0.5f, (geo.x[3] + geo.x[4]) * 0.5f,
                        (geo.x[4] + geo.x[5]) * 0.5f};
    for (int i = 0; i < 5; ++i)
        drawText(g, labels[i], juce::Rectangle<float>(xs[i] - 8.0f, area.getBottom() - 13.0f, 16.0f, 12.0f), kTextSmall, t.textDim, juce::Justification::centred);
}

void EnvelopeView::mouseMove(const juce::MouseEvent& e) {
    const auto plot = getLocalBounds().toFloat().reduced(14.0f, 12.0f);
    const auto geo = geometry();
    const float sustain = static_cast<float>(values_[4]);
    const juce::Point<float> handles[4] = {{geo.x[1], plot.getY()}, {geo.x[2], plot.getY()}, {geo.x[3], plot.getBottom() - plot.getHeight() * sustain}, {geo.x[5], plot.getBottom()}};
    int hit = -1;
    for (int i = 0; i < 4; ++i)
        if (handles[i].getDistanceFrom(e.position) < 9.0f)
            hit = i;
    if (hit != hoverHandle_) {
        hoverHandle_ = hit;
        repaint();
    }
}

void EnvelopeView::mouseDown(const juce::MouseEvent& e) {
    mouseMove(e);
    dragHandle_ = hoverHandle_;
    if (dragHandle_ < 0)
        return;
    static const char* const times[4] = {"attack", "hold", "decay", "release"};
    dragStart_ = ctx_.processor.plainValue(param(times[dragHandle_]));
    ctx_.session.beginGesture(param(times[dragHandle_]));
    if (dragHandle_ == 2)
        ctx_.session.beginGesture(param("sustain"));
}

void EnvelopeView::mouseDrag(const juce::MouseEvent& e) {
    if (dragHandle_ < 0)
        return;
    static const char* const times[4] = {"attack", "hold", "decay", "release"};
    const int p = param(times[dragHandle_]);
    const float fine = e.mods.isShiftDown() ? 0.2f : 1.0f;
    const double factor = std::exp(static_cast<double>(e.getDistanceFromDragStartX()) / 70.0 * fine);
    ctx_.session.setFromGesture(p, std::max(0.0, (dragStart_ + 1.0) * factor - 1.0)); // times grow and shrink by ratio
    if (dragHandle_ == 2) {
        const auto plot = getLocalBounds().toFloat().reduced(14.0f, 12.0f);
        ctx_.session.setFromGesture(param("sustain"), juce::jlimit(0.0, 1.0, static_cast<double>((plot.getBottom() - e.position.y) / plot.getHeight())));
    }
    tick();
    repaint();
}

void EnvelopeView::mouseUp(const juce::MouseEvent&) {
    if (dragHandle_ < 0)
        return;
    static const char* const times[4] = {"attack", "hold", "decay", "release"};
    ctx_.session.endGesture(param(times[dragHandle_]));
    if (dragHandle_ == 2)
        ctx_.session.endGesture(param("sustain"));
    dragHandle_ = -1;
    repaint();
}

// --- SpectrumView ------------------------------------------------------------------------------------

SpectrumView::SpectrumView(Context& ctx) : ctx_(ctx) {
    setTitle("Spectrum");
    setHelp("Spectrum", "The instrument's output, by frequency.");
    family = Family::Effects;
    setInterceptsMouseClicks(false, false);
}

bool SpectrumView::tick() {
    constexpr int kSize = 2048, kBins = 120;
    scratch_.resize(kSize);
    ctx_.processor.readOutput(scratch_.data(), kSize);
    std::vector<zed::dsp::Fft::Complex> data(kSize);
    float peak = 0.0f;
    for (int i = 0; i < kSize; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * zed::kPi * i / (kSize - 1));
        data[static_cast<std::size_t>(i)] = scratch_[static_cast<std::size_t>(i)] * w;
        peak = std::max(peak, std::abs(scratch_[static_cast<std::size_t>(i)]));
    }
    if (smoothed_.size() != kBins)
        smoothed_.assign(kBins, -120.0f);
    if (peak < 1e-6f) { // silence: settle, then stop repainting
        if (quietFrames_ > 40)
            return false;
        ++quietFrames_;
    } else {
        quietFrames_ = 0;
    }
    zed::dsp::Fft::forSize(kSize).forward(data.data());
    const double sr = ctx_.processor.currentSampleRate();
    for (int b = 0; b < kBins; ++b) {
        const double f0 = 20.0 * std::pow(1000.0, b / static_cast<double>(kBins)), f1 = 20.0 * std::pow(1000.0, (b + 1) / static_cast<double>(kBins));
        const int k0 = std::max(1, static_cast<int>(f0 / sr * kSize)), k1 = std::max(k0, std::min(kSize / 2 - 1, static_cast<int>(f1 / sr * kSize)));
        double power = 0.0;
        for (int k = k0; k <= k1; ++k)
            power = std::max(power, std::norm(data[static_cast<std::size_t>(k)]));
        const float db = static_cast<float>(10.0 * std::log10(power / (kSize * kSize * 0.0625) + 1e-14));
        float& s = smoothed_[static_cast<std::size_t>(b)];
        s = db > s ? db : s + (db - s) * 0.25f;
    }
    return true;
}

void SpectrumView::draw(juce::Graphics& g) {
    const Theme& t = theme();
    const auto area = getLocalBounds().toFloat();
    drawWell(g, area);
    const auto plot = area.reduced(8.0f, 8.0f).withTrimmedBottom(12.0f);
    g.setColour(t.displayGrid);
    for (const double f : {100.0, 1000.0, 10000.0}) {
        const float x = plot.getX() + plot.getWidth() * static_cast<float>(std::log(f / 20.0) / std::log(1000.0));
        g.fillRect(x, plot.getY(), 1.0f, plot.getHeight());
        drawText(g, formatHz(f), juce::Rectangle<float>(x - 20.0f, plot.getBottom() + 1.0f, 40.0f, 12.0f), kTextSmall, t.textDim, juce::Justification::centred);
    }
    if (!smoothed_.empty()) {
        juce::Path p;
        for (std::size_t b = 0; b < smoothed_.size(); ++b) {
            const float x = plot.getX() + plot.getWidth() * static_cast<float>(b) / static_cast<float>(smoothed_.size() - 1);
            const float y = plot.getBottom() - plot.getHeight() * juce::jlimit(0.0f, 1.0f, (smoothed_[b] + 90.0f) / 90.0f);
            if (b == 0) {
                p.startNewSubPath(x, plot.getBottom());
                p.lineTo(x, y);
            } else {
                p.lineTo(x, y);
            }
        }
        p.lineTo(plot.getRight(), plot.getBottom());
        p.closeSubPath();
        g.setColour(t.effects.withAlpha(0.25f));
        g.fillPath(p);
    }
    if (curve) {
        std::vector<double> hz;
        for (int i = 0; i < 160; ++i)
            hz.push_back(20.0 * std::pow(1000.0, i / 159.0));
        std::vector<float> db(hz.size());
        if (curve(hz, db)) {
            juce::Path p;
            for (std::size_t i = 0; i < hz.size(); ++i) {
                const float x = plot.getX() + plot.getWidth() * static_cast<float>(i) / 159.0f;
                const float y = plot.getCentreY() - plot.getHeight() * 0.5f * juce::jlimit(-1.0f, 1.0f, db[i] / 24.0f);
                if (i == 0)
                    p.startNewSubPath(x, y);
                else
                    p.lineTo(x, y);
            }
            strokeGlow(g, p, t.displayLine, 1.6f);
        }
    }
}

// --- SignalMap ---------------------------------------------------------------------------------------

SignalMap::SignalMap(Context& ctx) : ctx_(ctx) {
    setTitle("Signal map");
    setHelp("Signal map", "How the sound flows: sources into the filters or the direct bus, through the effect lanes to the output. Click a part to go to it.");
}

std::vector<SignalMap::Node> SignalMap::nodes() const {
    const auto laneCount = [&](int lane) {
        int n = 0;
        for (int s = 0; s < zed::kNumFxSlots; ++s)
            n += ctx_.processor.engine().fxSlotStatus(lane, s).present ? 1 : 0;
        return n;
    };
    const auto area = getLocalBounds().toFloat().reduced(10.0f);
    const auto v = [&](const char* id) { return ctx_.processor.plainValue(idx(id)); };
    const auto db = [&](const char* id) { return str(zed::formatValueText(*reg().find(id), v(id))); };
    std::vector<Node> out;
    const float colW = area.getWidth() / 5.0f;
    const auto column = [&](int c, int row, int rows, float h = 34.0f) {
        const float cx = area.getX() + colW * (static_cast<float>(c) + 0.5f);
        const float cy = area.getY() + area.getHeight() * (static_cast<float>(row) + 0.5f) / static_cast<float>(rows);
        return juce::Rectangle<float>(std::min(colW - 14.0f, 96.0f), h).withCentre({cx, cy});
    };
    const char* src[] = {"osc1", "osc2", "osc3", "sub", "texture"};
    const char* names[] = {"Osc 1", "Osc 2", "Osc 3", "Sub", "Texture"};
    for (int i = 0; i < 5; ++i) {
        const juce::String m(src[i]);
        out.push_back({m, names[i], column(0, i, 5), v((m + ".on").toRawUTF8()) >= 0.5, db((m + ".level").toRawUTF8()), Family::Sources});
    }
    out.push_back({"filter1", "Filter 1", column(1, 0, 3), v("filter1.on") >= 0.5, str(zed::formatValueText(*reg().find("filter1.cutoff"), v("filter1.cutoff"))), Family::Filters});
    out.push_back({"filter2", "Filter 2", column(1, 1, 3), v("filter2.on") >= 0.5, str(zed::formatValueText(*reg().find("filter2.cutoff"), v("filter2.cutoff"))), Family::Filters});
    out.push_back({"voice", "Direct", column(1, 2, 3), true, "", Family::Neutral});
    out.push_back({"env1", "Amp", column(2, 0, 1), true, "Env 1", Family::Modulators});
    out.push_back({"fx.a", "Lane A", column(3, 0, 2), true, juce::String(laneCount(0)) + " fx", Family::Effects});
    out.push_back({"fx.b", "Lane B", column(3, 1, 2), true, v("fx.b_routing") >= 0.5 ? "series" : "parallel", Family::Effects});
    out.push_back({"global", "Output", column(4, 0, 1), v("global.fx_off") < 0.5, db("global.master"), Family::Neutral});
    return out;
}

bool SignalMap::tick() {
    std::uint64_t h = 0;
    for (const char* id : {"osc1.on", "osc2.on", "osc3.on", "sub.on", "texture.on", "osc1.level", "osc2.level", "osc3.level", "sub.level",
                           "texture.level", "filter1.on", "filter2.on", "filter1.cutoff", "filter2.cutoff", "voice.filter_routing",
                           "osc1.sends.direct", "osc2.sends.direct", "osc3.sends.direct", "osc1.sends.filter_balance", "osc2.sends.filter_balance",
                           "osc3.sends.filter_balance", "sub.direct", "texture.direct", "fx.b_routing", "global.master", "global.fx_off"})
        h = mix(h, ctx_.processor.plainValue(idx(id)));
    h = mix(h, static_cast<double>(ctx_.processor.stateGeneration()));
    if (h == key_)
        return false;
    key_ = h;
    return true;
}

void SignalMap::draw(juce::Graphics& g) {
    const Theme& t = theme();
    const auto area = getLocalBounds().toFloat();
    drawWell(g, area);
    const auto list = nodes();
    const auto v = [&](const char* id) { return ctx_.processor.plainValue(idx(id)); };
    const auto edge = [&](const Node& a, const Node& b, double weight, juce::Colour c) {
        if (weight <= 0.001)
            return;
        juce::Path p;
        const auto p0 = juce::Point<float>(a.box.getRight(), a.box.getCentreY());
        const auto p1 = juce::Point<float>(b.box.getX(), b.box.getCentreY());
        const float mid = (p0.x + p1.x) * 0.5f;
        p.startNewSubPath(p0);
        p.cubicTo(mid, p0.y, mid, p1.y, p1.x, p1.y);
        g.setColour(c.withAlpha(a.on && b.on ? 0.75f : 0.25f));
        g.strokePath(p, juce::PathStrokeType(0.8f + 2.6f * static_cast<float>(weight)));
    };
    // Sources into the filters and the direct bus.
    for (int i = 0; i < 5; ++i) {
        double direct = 0.0, balance = 0.0;
        if (i < 3) {
            const juce::String m = "osc" + juce::String(i + 1);
            direct = v((m + ".sends.direct").toRawUTF8());
            balance = v((m + ".sends.filter_balance").toRawUTF8());
        } else {
            const char* m = i == 3 ? "sub" : "texture";
            direct = v((juce::String(m) + ".direct").toRawUTF8()) >= 0.5 ? 1.0 : 0.0;
            balance = v((juce::String(m) + ".sends.filter_balance").toRawUTF8());
        }
        edge(list[static_cast<std::size_t>(i)], list[5], (1.0 - direct) * (1.0 - balance), t.sources);
        edge(list[static_cast<std::size_t>(i)], list[6], (1.0 - direct) * balance, t.sources);
        edge(list[static_cast<std::size_t>(i)], list[7], direct, t.sources);
    }
    const double serial = 1.0 - v("voice.filter_routing");
    if (serial > 0.001) { // filter 1 into filter 2
        g.setColour(t.filters.withAlpha(0.7f));
        g.drawArrow(juce::Line<float>(list[5].box.getCentreX(), list[5].box.getBottom(), list[6].box.getCentreX(), list[6].box.getY()),
                    0.8f + 2.0f * static_cast<float>(serial), 6.0f, 6.0f);
    }
    for (int i = 5; i < 8; ++i)
        edge(list[static_cast<std::size_t>(i)], list[8], 0.6, t.filters);
    edge(list[8], list[9], 0.6, t.effects);
    edge(list[8], list[10], 0.4, t.effects);
    edge(list[9], list[11], 0.6, t.effects);
    edge(list[10], list[11], 0.6, t.effects);
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
        const auto& n = list[static_cast<std::size_t>(i)];
        g.setColour(i == hover_ ? t.raised.brighter(0.1f) : t.raised);
        g.fillRoundedRectangle(n.box, 5.0f);
        g.setColour(n.on ? t.accent(n.family) : t.line);
        g.drawRoundedRectangle(n.box.reduced(0.5f), 5.0f, i == hover_ ? 1.6f : 1.0f);
        auto text = n.box.reduced(4.0f, 2.0f);
        drawText(g, n.name, text.removeFromTop(n.level.isEmpty() ? text.getHeight() : 15.0f), kTextSmall, n.on ? t.text : t.textFaint,
                 juce::Justification::centred, true);
        if (n.level.isNotEmpty())
            drawText(g, n.level, text, kTextSmall, t.textDim, juce::Justification::centred);
    }
}

void SignalMap::mouseMove(const juce::MouseEvent& e) {
    const auto list = nodes();
    int hit = -1;
    for (int i = 0; i < static_cast<int>(list.size()); ++i)
        if (list[static_cast<std::size_t>(i)].box.contains(e.position))
            hit = i;
    if (hit != hover_) {
        hover_ = hit;
        repaint();
    }
}

void SignalMap::mouseDown(const juce::MouseEvent& e) {
    for (const auto& n : nodes())
        if (n.box.contains(e.position) && ctx_.reveal)
            ctx_.reveal(n.module);
}

} // namespace zui
