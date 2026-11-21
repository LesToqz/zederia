#include "Displays.h"

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

// Quarter-cycle divisions and a stronger zero baseline keep the phase and sign of a cycle visible.
void drawScopeGrid(juce::Graphics& g, juce::Rectangle<float> plot) {
    const auto& t = theme();
    g.setColour(t.displayGrid.withAlpha(0.65f));
    for (int i = 1; i < 4; ++i) {
        const float x = plot.getX() + plot.getWidth() * static_cast<float>(i) * 0.25f;
        g.fillRect(x, plot.getY(), 1.0f, plot.getHeight());
    }
    g.setColour(t.lineStrong.withAlpha(0.35f));
    g.fillRect(plot.getX(), plot.getCentreY(), plot.getWidth(), 1.0f);
    for (int i = 0; i <= 16; ++i) {
        const float x = plot.getX() + plot.getWidth() * static_cast<float>(i) / 16.0f;
        g.fillRect(x, plot.getCentreY() - 1.5f, 1.0f, 4.0f);
    }
}

// All slices, including the live one, use the same oblique projection. The width tapers
// towards the back, so changes in table position read independently of waveform amplitude.
struct TableProjection {
    explicit TableProjection(juce::Rectangle<float> scope)
        : area(scope.reduced(12.0f, 5.0f).withTrimmedTop(17.0f).withTrimmedBottom(6.0f)) {}
    juce::Point<float> point(float phase, float depth, float sample) const {
        const float x = 0.14f * depth + phase * (0.91f - 0.06f * depth);
        const float y = 0.70f - depth * 0.42f - phase * 0.035f - juce::jlimit(-1.2f, 1.2f, sample) * 0.245f;
        return {area.getX() + x * area.getWidth(), area.getY() + y * area.getHeight()};
    }
    juce::Rectangle<float> area;
};

void drawFrequencyAxis(juce::Graphics& g, juce::Rectangle<float> plot) {
    const auto& t = theme();
    for (const double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
        const float x = plot.getX() + plot.getWidth() * static_cast<float>(std::log10(f / 20.0) / 3.0);
        const bool decade = f == 100.0 || f == 1000.0 || f == 10000.0;
        g.setColour(t.displayGrid.withAlpha(decade ? 0.9f : 0.35f));
        g.fillRect(x, plot.getY(), 1.0f, plot.getHeight());
        if (decade) {
            g.setColour(t.lineStrong);
            g.fillRect(x, plot.getBottom(), 1.0f, 2.5f);
            drawText(g, formatHz(f), {x - 20.0f, plot.getBottom() + 2.0f, 40.0f, 12.0f},
                     kTextSmall, t.textDim, juce::Justification::centred);
        }
    }
}

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

bool WaveMini::sourceOn() const {
    const char* on[] = {"osc1.on", "osc2.on", "osc3.on", "sub.on", "texture.on"};
    return ctx_.processor.plainValue(idx(on[source_])) >= 0.5;
}

// Every other frame: a thumbnail this small reads the same at half the rate.
bool WaveMini::tick() {
    if ((++ticks_ & 1) != 0)
        return false;
    const bool changed = rebuild();
    const bool on = sourceOn();
    if (on == active_)
        return changed;
    active_ = on; // what the next paint draws
    return true;
}

void WaveMini::draw(juce::Graphics& g) {
    if (cycle_.empty())
        rebuild();
    const Theme& t = theme();
    const auto area = getLocalBounds().toFloat();
    g.setColour(t.display);
    g.fillRoundedRectangle(area, 3.0f);
    const bool active = sourceOn();
    active_ = active;
    g.setColour(t.displayGrid);
    g.fillRect(area.getX(), area.getCentreY(), area.getWidth(), 1.0f);
    drawCycleLine(g, cycle_, area.reduced(3.0f, 2.0f), active ? t.displayLine : t.textFaint.withAlpha(0.7f), active ? 1.2f : 0.9f);
}

// --- WavetableView -----------------------------------------------------------------------------------

WavetableView::WavetableView(Context& ctx, int osc) : ctx_(ctx), osc_(osc) {
    bindParams();
    setOpaque(true); // it covers its whole area: nothing behind it repaints when it animates
    setTitle("Wavetable");
    setHelp("Wavetable", "The oscillator's table: frames from front to back, the playing position lit. The 3D / 2D button in the corner switches the view.");
    family = Family::Sources;
    viewButton_.textSize = kTextSmall;
    viewButton_.boldText = true;
    viewButton_.setHelp("Table view", "3D shows the frames of the table in depth with the playing position lit; 2D the cycle that plays now.");
    viewButton_.onClick = [this] { setShow3d(!show3d); };
    addAndMakeVisible(viewButton_);
    updateViewButton();
}

bool WavetableView::oscOn() const { return ctx_.processor.plainValue(onParam_) >= 0.5; }
bool WavetableView::classic() const { return ctx_.processor.plainValue(engineParam_) >= 0.5; }

// The parameters the view reads every frame, looked up once per oscillator.
void WavetableView::bindParams() {
    const juce::String m = "osc" + juce::String(osc_ + 1);
    onParam_ = idx(m + ".on");
    engineParam_ = idx(m + ".engine");
    positionParam_ = idx(m + ".position");
}
bool WavetableView::showing3d() const { return show3d && !classic(); }

void WavetableView::setShow3d(bool on) {
    show3d = on;
    warpKey_ = 0;
    updateViewButton();
    repaint();
}

void WavetableView::updateViewButton() {
    // The control names the view that shows; Classic has only its cycle, so it hides there.
    const juce::String shown = show3d ? "3D" : "2D";
    viewButton_.setText(shown);
    viewButton_.setTitle("Osc " + juce::String(osc_ + 1) + " table view: " + (show3d ? "3D" : "2D cycle") + " (switch)");
    viewButton_.setVisible(!classicShown_);
}

void WavetableView::resized() {
    cache_ = {};
    viewButton_.setBounds(getLocalBounds().reduced(5, 4).removeFromTop(18).removeFromLeft(34));
}

void WavetableView::setOsc(int osc) {
    osc_ = osc;
    bindParams();
    cache_ = {};
    cachedTable_ = nullptr;
    warpKey_ = 0;
    updateViewButton();
    repaint();
}

bool WavetableView::tick() {
    const double position = ctx_.processor.engine().liveValue(positionParam_);
    const auto table = ctx_.processor.engine().table(osc_);
    const bool classic = this->classic();
    const bool active = oscOn(); // the power switch: host automation changes the look with nothing else moving
    bool whole = table.get() != tickedTable_ || classic != classicShown_ || active != activeShown_;
    if (classic != classicShown_) {
        classicShown_ = classic;
        updateViewButton();
    }
    tickedTable_ = table.get();
    activeShown_ = active;
    bool moved = false;
    if (std::abs(position - position_) > 0.0008) {
        position_ = position;
        moved = true;
    }
    const int percent = juce::roundToInt(juce::jlimit(0.0, 1.0, ctx_.processor.plainValue(positionParam_)) * 100.0);
    const bool captionChanged = percent != captionPercent_ && !classic;
    captionPercent_ = percent;
    if (captionChanged && !moved && !whole && show3d && !classic) {
        dirty_ = juce::RectangleList<int>(captionArea());
        return true;
    }
    if (!show3d || classic) {
        whole = oscCycle(ctx_, osc_, cycle_, warpKey_) || whole || moved || captionChanged;
    } else if (moved && !whole && cache_.isValid() && table != nullptr && litDrawn_.size() == static_cast<std::size_t>(kLitPoints + 1)) {
        // Only the lit line moved: repaint the band where it was and where it goes. (Narrow slices
        // along the line repaint fewer pixels but cost more: every drawing call then works
        // through each slice.)
        const auto next = litPoints(*table, position_);
        float left = 1.0e9f, top = 1.0e9f, right = -1.0e9f, bottom = -1.0e9f;
        for (std::size_t i = 0; i < next.size(); ++i)
            for (const auto p : {litDrawn_[i], next[i]}) {
                left = std::min(left, p.x);
                right = std::max(right, p.x);
                top = std::min(top, p.y);
                bottom = std::max(bottom, p.y);
            }
        // The stroke, its smoothing and the phase-origin marker.
        dirty_ = juce::RectangleList<int>(juce::Rectangle<float>::leftTopRightBottom(left, top, right, bottom).getSmallestIntegerContainer().expanded(4));
        if (captionChanged)
            dirty_.add(captionArea());
        return true;
    } else {
        whole = whole || moved || captionChanged;
    }
    dirty_ = juce::RectangleList<int>(getLocalBounds());
    return whole;
}

std::vector<juce::Point<float>> WavetableView::litPoints(const zed::TableData& table, double position) const {
    const TableProjection projection(getLocalBounds().toFloat());
    const int frames = table.frames();
    const float depth = frames > 1 ? static_cast<float>(juce::jlimit(0.0, 1.0, position)) : 0.0f;
    const double framePos = depth * (frames - 1);
    std::vector<juce::Point<float>> points(static_cast<std::size_t>(kLitPoints + 1));
    for (int i = 0; i <= kLitPoints; ++i) {
        const float v = static_cast<float>(table.read(9, framePos, static_cast<double>(i % kLitPoints) / kLitPoints));
        points[static_cast<std::size_t>(i)] = projection.point(static_cast<float>(i) / kLitPoints, depth, v);
    }
    return points;
}

// The table's surface, drawn once into an opaque image at the display's pixel density (the
// background and frame included), so a frame of animation is one plain copy plus the lit line.
// On: warm layers that fade towards the back. Off: the same layers in thin graphite.
void WavetableView::build3d(juce::Rectangle<int> size, bool active) {
    const auto table = ctx_.processor.engine().table(osc_);
    cachedTable_ = table.get();
    cachedTheme_ = theme().name;
    cacheActive_ = active;
    const float scale = static_cast<float>(size.getWidth()) / static_cast<float>(juce::jmax(1, getWidth()));
    cache_ = juce::Image(juce::Image::RGB, juce::jmax(1, size.getWidth()), juce::jmax(1, size.getHeight()), true, juce::SoftwareImageType());
    juce::Graphics g(cache_);
    g.addTransform(juce::AffineTransform::scale(scale));
    const Theme& t = theme();
    g.fillAll(t.bg);
    drawWell(g, getLocalBounds().toFloat());
    if (table == nullptr)
        return;
    const TableProjection projection(getLocalBounds().toFloat());
    const int frames = table->frames();
    // The floor is the negative amplitude limit. All ridges above it are sampled from the
    // table. Cross-sections coincide with stored frames when the frame count permits.
    g.setColour(t.displayGrid.withAlpha(0.75f));
    for (int phase = 0; phase <= 4; ++phase)
        g.drawLine({projection.point(phase / 4.0f, 0.0f, -1.0f), projection.point(phase / 4.0f, 1.0f, -1.0f)}, 0.75f);
    for (const float depth : {0.0f, 0.5f, 1.0f})
        g.drawLine({projection.point(0.0f, depth, -1.0f), projection.point(1.0f, depth, -1.0f)}, 0.75f);

    constexpr int samples = kLitPoints;
    const int intervals = juce::jlimit(1, 24, frames - 1);
    const int strips = frames <= 1 ? 0 : intervals * juce::jmax(1, 24 / intervals);
    const int stride = strips > 0 ? strips / intervals : 1;
    std::array<juce::Point<float>, samples + 1> back{}, front{};
    const auto crest = active ? t.sources : t.textFaint;
    for (int l = strips; l >= 0; --l) {
        const float depth = strips > 0 ? static_cast<float>(l) / strips : 0.0f;
        juce::Path ridge;
        for (int i = 0; i <= samples; ++i) {
            const float v = static_cast<float>(table->read(9, depth * (frames - 1), static_cast<double>(i % samples) / samples));
            front[i] = projection.point(static_cast<float>(i) / samples, depth, v);
            if (i == 0) ridge.startNewSubPath(front[i]);
            else ridge.lineTo(front[i]);
        }
        if (l < strips) {
            juce::Path skin(ridge);
            for (int i = samples; i >= 0; --i)
                skin.lineTo(back[i]);
            skin.closeSubPath();
            const auto low = t.display.interpolatedWith(crest, active ? 0.13f : 0.055f);
            const auto high = t.display.interpolatedWith(crest, active ? 0.62f : 0.19f);
            g.setGradientFill({high, projection.area.getTopLeft(), low, projection.area.getBottomLeft(), false});
            g.fillPath(skin);
            g.setColour(crest.withAlpha(active ? 0.32f : 0.14f));
            for (int i = 0; i <= samples; i += samples / 8)
                g.drawLine({front[i], back[i]}, 0.7f);
        }
        const bool keyFrame = frames <= 25 && l % stride == 0;
        g.setColour(crest.withAlpha(keyFrame ? (active ? 0.92f : 0.48f) : (active ? 0.55f : 0.23f)));
        g.strokePath(ridge, juce::PathStrokeType(keyFrame ? 1.1f : 0.75f, juce::PathStrokeType::beveled));
        back.swap(front);
    }
}

void WavetableView::drawCycle(juce::Graphics& g, juce::Rectangle<float> area, bool active) {
    if (cycle_.empty())
        oscCycle(ctx_, osc_, cycle_, warpKey_);
    const Theme& t = theme();
    const auto scope = area.reduced(6.0f, 3.0f).withTrimmedTop(classic() ? 0.0f : 13.0f);
    drawScopeGrid(g, scope);
    drawCycleLine(g, cycle_, scope, active ? t.displayLine : t.textFaint.withAlpha(0.55f), active ? 1.8f : 1.0f);
}

void WavetableView::draw(juce::Graphics& g) {
    const Theme& t = theme();
    const auto area = getLocalBounds().toFloat();
    plot_ = area.reduced(6.0f);
    const bool classicNow = classic();
    const bool active = oscOn();
    activeShown_ = active;
    if (!show3d || classicNow) {
        g.fillAll(t.bg); // opaque: the corners outside the rounded well
        drawWell(g, area);
        drawCycle(g, plot_, active);
    } else {
        const float scale = physicalScale(g);
        const auto size = juce::Rectangle<int>(juce::roundToInt(area.getWidth() * scale), juce::roundToInt(area.getHeight() * scale));
        const auto table = ctx_.processor.engine().table(osc_);
        if (!cache_.isValid() || cache_.getBounds() != size || table.get() != cachedTable_ || cachedTheme_ != t.name || cacheActive_ != active)
            build3d(size, active);
        g.drawImage(cache_, area); // one pixel of the image per pixel of the display: a plain copy
        // The playing position across the surface: crisp and white while the oscillator sounds, a
        // thin graphite line while it is off.
        if (table != nullptr) {
            const auto footer = getLocalBounds().reduced(7, 2).removeFromBottom(15);
            if (g.clipRegionIntersects(footer)) {
                const int frames = table->frames();
                drawText(g, juce::String(frames) + (frames == 1 ? " frame" : " frames"), footer.toFloat(), kTextSmall,
                         t.textDim, juce::Justification::centredRight);
            }
            // A plain line, no glow: it moves every frame, and a wide translucent stroke across the
            // surface costs more to fill than the rest of the frame together.
            litDrawn_ = litPoints(*table, position_);
            juce::Path p;
            p.startNewSubPath(litDrawn_.front());
            for (std::size_t i = 1; i < litDrawn_.size(); ++i)
                p.lineTo(litDrawn_[i]);
            g.setColour(active ? t.displayLine : t.textFaint.withAlpha(0.6f));
            g.strokePath(p, juce::PathStrokeType(active ? 2.0f : 1.0f, juce::PathStrokeType::beveled, juce::PathStrokeType::butt));
            if (active)
                g.fillEllipse(juce::Rectangle<float>(4.5f, 4.5f).withCentre(litDrawn_.front()));
        }
    }
    // The patch's position (the lit line shows where modulation takes it); Classic has none.
    if (!classicNow) {
        const double setPosition = juce::jlimit(0.0, 1.0, ctx_.processor.plainValue(positionParam_));
        const auto caption = captionArea().toFloat();
        drawText(g, juce::String(juce::roundToInt(setPosition * 100.0)) + " %", caption, kTextSmall, t.textDim,
                 juce::Justification::centredRight);
    }
}

// --- FilterResponseView ------------------------------------------------------------------------------

FilterResponseView::FilterResponseView(Context& ctx, int slot) : ctx_(ctx), slot_(slot) {
    setTitle("Filter response");
    setOpaque(true); // it paints every pixel: nothing behind it repaints with its curves
    setHelp("Filter response", "The faint curve follows the patch settings and the bright curve follows modulation. Click to switch between magnitude and phase.");
    family = Family::Filters;
    for (int i = 0; i < 180; ++i)
        hz_.push_back(20.0 * std::pow(1000.0, i / 179.0));
}

void FilterResponseView::setSlot(int slot) {
    slot_ = slot;
    tick();
    repaint();
}

void FilterResponseView::togglePhase() {
    showPhase = !showPhase;
    tick();
    repaint();
    if (onPhaseChanged)
        onPhaseChanged();
}

void FilterResponseView::mouseDown(const juce::MouseEvent&) { togglePhase(); }

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
        // Field by field: the struct's padding bytes are not part of its value.
        for (const double field : {static_cast<double>(v.settings.type), static_cast<double>(v.settings.cutoffHz[0]), static_cast<double>(v.settings.cutoffHz[1]),
                                   static_cast<double>(v.settings.resonance), static_cast<double>(v.settings.drive), static_cast<double>(v.settings.character),
                                   static_cast<double>(v.settings.mix), v.settings.levelMode ? 1.0 : 0.0})
            h = mix(h, field);
        h = mix(h, v.on ? 1.0 : 0.0);
        h = mix(h, static_cast<double>(v.oversample));
    }
    std::uint64_t patch = static_cast<std::uint64_t>(slot_ + 1) * 131u + (showPhase ? 7u : 0u);
    patch = mix(patch, engine.sampleRate()); // the response depends on the rate and the oversampling too
    for (const char* f : {"type", "cutoff", "resonance", "drive", "character", "mix", "mix_mode", "on"})
        patch = mix(patch, ctx_.processor.plainValue(idx("filter" + juce::String(slot_ + 1) + "." + f)));
    h = mix(h, static_cast<double>(patch));
    if (h == key_)
        return false;
    // Movement from modulation alone redraws at a third of the frame rate; a change to the patch at once.
    if (patch == patchKey_ && (++frames_ % 3u) != 0)
        return false;
    patchKey_ = patch;
    key_ = h;
    compute();
    return true;
}

void FilterResponseView::draw(juce::Graphics& g) {
    if (still_.empty())
        compute();
    const Theme& t = theme();
    const auto area = getLocalBounds().toFloat();
    const auto plot = area.reduced(8.0f).withTrimmedLeft(25.0f).withTrimmedTop(12.0f).withTrimmedBottom(12.0f);
    const float lo = showPhase ? -3.2f : -36.0f, hi = showPhase ? 3.2f : 24.0f;
    const auto xOf = [&](std::size_t i) { return plot.getX() + plot.getWidth() * static_cast<float>(i) / static_cast<float>(hz_.size() - 1); };
    const auto yOf = [&](float v) { return plot.getBottom() - plot.getHeight() * (juce::jlimit(lo, hi, v) - lo) / (hi - lo); };
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
    // The well, the grid, the patch's own response and the caption change only with the settings:
    // kept as an image, so a modulated cutoff repaints just the live curves over it.
    const double cutoff = ctx_.processor.plainValue(idx("filter" + juce::String(slot_ + 1) + ".cutoff"));
    const std::uint64_t key = mix(zed::fnv1a(still_.data(), still_.size() * sizeof(float),
                                         static_cast<std::uint64_t>(slot_ * 4 + (showPhase ? 2 : 0) + (on_[slot_] ? 1 : 0))), cutoff);
    staticLayer_.draw(g, getLocalBounds(), key, true, [&](juce::Graphics& sg) {
        sg.fillAll(t.panel); // the corners outside the rounded well
        drawWell(sg, area);
        drawFrequencyAxis(sg, plot);
        for (const float db : showPhase ? std::vector<float>{-1.5708f, 0.0f, 1.5708f} : std::vector<float>{-24.0f, -12.0f, 0.0f, 12.0f}) {
            sg.setColour(db == 0.0f ? t.lineStrong.withAlpha(0.75f) : t.displayGrid);
            sg.fillRect(plot.getX(), yOf(db), plot.getWidth(), 1.0f);
            const int val = juce::roundToInt(showPhase ? db * (180.0f / juce::MathConstants<float>::pi) : db);
            const juce::String label = (val > 0 ? "+" : "") + juce::String(val);
            drawText(sg, label, {2.0f, yOf(db) - 7.0f, plot.getX() - 7.0f, 14.0f},
                     kTextSmall, t.textDim, juce::Justification::centredRight);
        }
        const juce::String unit = showPhase ? juce::String::fromUTF8("°") : juce::String("dB");
        drawText(sg, unit, {2.0f, 3.0f, plot.getX() - 7.0f, 14.0f}, kTextSmall, t.textDim, juce::Justification::centredRight, true);
        const float cutX = plot.getX() + plot.getWidth() * static_cast<float>(std::log10(juce::jlimit(20.0, 20000.0, cutoff) / 20.0) / 3.0);
        sg.setColour((on_[slot_] ? t.sources : t.textFaint).withAlpha(0.75f));
        juce::Path marker;
        marker.addTriangle(cutX - 3.0f, plot.getY() - 4.0f, cutX + 3.0f, plot.getY() - 4.0f, cutX, plot.getY());
        sg.fillPath(marker);
        sg.setColour((on_[slot_] ? t.sources : t.textFaint).withAlpha(0.22f));
        const float dash[] = {2.0f, 3.0f};
        sg.drawDashedLine({{cutX, plot.getY()}, {cutX, plot.getBottom()}}, dash, 2);

        // The cutoff pointer and dim response are the patch settings. The bright curve below
        // follows the playing voice including modulation, key tracking and bypass.
        sg.setColour(t.control.withAlpha(on_[slot_] ? 0.42f : 0.37f));
        sg.strokePath(path(still_), juce::PathStrokeType(1.0f));
        if (!on_[slot_])
            drawText(sg, "Bypassed", plot.withY(2.0f).withHeight(14.0f), kTextSmall, t.textDim, juce::Justification::centredLeft);
    });
    const int other = 1 - slot_;
    const auto& otherValues = showPhase ? phase_[other] : live_[other];
    if (on_[other] && !otherValues.empty()) {
        g.setColour(t.textDim.withAlpha(0.3f));
        g.strokePath(path(otherValues), juce::PathStrokeType(1.0f));
    }
    const auto& mine = showPhase ? phase_[slot_] : live_[slot_];
    if (!mine.empty()) { // a plain line: it moves every frame under modulation, and a glow there costs more than the rest
        g.setColour(on_[slot_] ? t.displayLine : t.textFaint.withAlpha(0.6f));
        g.strokePath(path(mine), juce::PathStrokeType(on_[slot_] ? 1.8f : 1.0f, juce::PathStrokeType::beveled, juce::PathStrokeType::rounded));
    }
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
    static const char* const names[] = {"delay", "attack", "hold", "decay", "sustain", "release", "attack_curve", "decay_curve", "release_curve"};
    for (int i = 0; i < static_cast<int>(std::size(names)); ++i) {
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
    segment(geo.x[4], geo.x[5], sustain, 0.0f, values_[8]);
    juce::Path fill(p);
    fill.lineTo(geo.x[5], yOf(0.0f));
    fill.closeSubPath();
    // The graph in its source's colour: the colour of its chip and of the arcs it draws on the controls it moves.
    const juce::Colour ink = ctx_.sourceColour("env" + juce::String(env_ + 1));
    g.setColour(t.displayGrid.withAlpha(0.72f));
    for (int i = 0; i < 5; ++i)
        g.fillRect(geo.x[i], plot.getY(), 1.0f, plot.getHeight());
    g.setColour(t.displayGrid);
    g.fillRect(plot.getX(), yOf(0.5f), plot.getWidth(), 0.8f);
    g.setColour(t.lineStrong);
    for (int i = 0; i <= 4; ++i)
        g.fillRect(plot.getX() - 6.0f, yOf(i * 0.25f), i % 2 == 0 ? 5.0f : 2.5f, 1.0f);
    g.setGradientFill({ink.withAlpha(0.27f), plot.getTopLeft(), ink.withAlpha(0.025f), plot.getBottomLeft(), false});
    g.fillPath(fill);
    strokeGlow(g, p, ink.interpolatedWith(t.displayLine, 0.17f), 1.8f);
    const juce::Point<float> handles[4] = {{geo.x[1], yOf(1.0f)}, {geo.x[2], yOf(1.0f)}, {geo.x[3], yOf(sustain)}, {geo.x[5], yOf(0.0f)}};
    for (int i = 0; i < 4; ++i) {
        const bool selected = i == hoverHandle_ || i == dragHandle_;
        const float side = selected ? 9.0f : 6.0f;
        const auto handle = juce::Rectangle<float>(side, side).withCentre(handles[i]);
        g.setColour(selected ? ink : t.display);
        g.fillRoundedRectangle(handle, 1.0f);
        g.setColour(selected ? t.displayLine : ink);
        g.drawRoundedRectangle(handle, 1.0f, 1.3f);
    }
    if (level_ > 0.001f) {
        const float y = yOf(juce::jlimit(0.0f, 1.0f, level_));
        g.setColour(t.displayLine.withAlpha(0.4f));
        g.fillRect(plot.getX(), y, plot.getWidth(), 1.0f);
        juce::Path marker;
        marker.addTriangle(plot.getX() - 7.0f, y - 3.0f, plot.getX() - 7.0f, y + 3.0f, plot.getX() - 2.0f, y);
        g.setColour(t.displayLine);
        g.fillPath(marker);
    }
    static const char* const labels[] = {"Attack", "Hold", "Decay", "Sustain", "Release"};
    for (int i = 0; i < 5; ++i) {
        const juce::Rectangle<float> legend(geo.x[i], area.getBottom() - 13.0f, geo.x[i + 1] - geo.x[i], 12.0f);
        const juce::String name(labels[i]);
        const auto label = legend.getWidth() > textWidth(name, kTextSmall) + 6.0f ? name : name.substring(0, 1);
        if (legend.getWidth() >= textWidth(label, kTextSmall) + 1.0f)
            drawText(g, label, legend, kTextSmall, t.textDim, juce::Justification::centred);
    }
    const int pointed = dragHandle_ >= 0 ? dragHandle_ : hoverHandle_;
    if (pointed >= 0 && plot.getWidth() >= 160.0f) {
        static const char* const params[] = {"attack", "hold", "decay", "release"};
        const int index = param(params[pointed]);
        const auto& def = ctx_.processor.effectiveDef(index);
        const juce::String value = str(def.name) + "  " + str(zed::formatValueText(def, ctx_.processor.plainValue(index)));
        const float w = juce::jmin(plot.getWidth(), textWidth(value, kTextSmall, true) + 10.0f);
        const bool left = handles[pointed].x > plot.getCentreX();
        const juce::Rectangle<float> box(left ? plot.getX() : plot.getRight() - w, plot.getCentreY() - 8.0f, w, 16.0f);
        g.setColour(t.display.withAlpha(0.94f));
        g.fillRect(box);
        drawText(g, value, box.reduced(4.0f, 0.0f), kTextSmall, t.text, juce::Justification::centred, true);
    }
}

void EnvelopeView::mouseMove(const juce::MouseEvent& e) {
    const auto plot = getLocalBounds().toFloat().reduced(14.0f, 12.0f);
    const auto geo = geometry();
    const float sustain = static_cast<float>(values_[4]);
    const juce::Point<float> handles[4] = {{geo.x[1], plot.getY()}, {geo.x[2], plot.getY()}, {geo.x[3], plot.getBottom() - plot.getHeight() * sustain}, {geo.x[5], plot.getBottom()}};
    int hit = -1;
    float distance = 9.0f;
    for (int i = 0; i < 4; ++i) {
        const float fromPoint = handles[i].getDistanceFrom(e.position);
        if (fromPoint < distance) {
            hit = i;
            distance = fromPoint;
        }
    }
    if (hit != hoverHandle_) {
        hoverHandle_ = hit;
        repaint();
    }
}

void EnvelopeView::mouseExit(const juce::MouseEvent&) {
    if (hoverHandle_ >= 0) {
        hoverHandle_ = -1;
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
    for (int i = 0; i < kSize; ++i)
        window_[i] = 0.5 - 0.5 * std::cos(2.0 * zed::kPi * i / (kSize - 1));
    smoothed_.assign(kBins, -120.0f);
    responseDb_.resize(160);
    for (int i = 0; i < 160; ++i)
        responseHz_.push_back(20.0 * std::pow(1000.0, i / 159.0));
}

bool SpectrumView::tick() {
    ctx_.processor.readOutput(scratch_.data(), kSize);
    float peak = 0.0f;
    for (const float sample : scratch_)
        peak = std::max(peak, std::abs(sample));
    if (peak < 1e-6f) { // silence: settle, then stop repainting
        if (quietFrames_ > 40)
            return false;
        ++quietFrames_;
    } else {
        quietFrames_ = 0;
    }
    for (int i = 0; i < kSize; ++i)
        fft_[i] = scratch_[i] * window_[i];
    zed::dsp::Fft::forSize(kSize).forward(fft_.data());
    const double sr = ctx_.processor.currentSampleRate();
    if (sr <= 0.0)
        return false;
    if (sr != binsRate_) {
        binsRate_ = sr;
        for (int i = 0; i <= kBins; ++i) {
            const double hz = 20.0 * std::pow(1000.0, static_cast<double>(i) / kBins);
            edges_[i] = juce::jlimit(1, kSize / 2, static_cast<int>(hz / sr * kSize));
        }
    }
    for (int b = 0; b < kBins; ++b) {
        double power = 0.0;
        for (int k = edges_[b]; k <= std::min(kSize / 2 - 1, edges_[b + 1]); ++k)
            power = std::max(power, std::norm(fft_[k]));
        const float db = static_cast<float>(10.0 * std::log10(power / (kSize * kSize * 0.0625) + 1e-14));
        float& s = smoothed_[static_cast<std::size_t>(b)];
        s = db > s ? db : s + (db - s) * 0.25f;
    }
    return true;
}

void SpectrumView::draw(juce::Graphics& g) {
    const Theme& t = theme();
    const auto area = getLocalBounds().toFloat();
    const auto plot = area.reduced(8.0f).withTrimmedLeft(28.0f).withTrimmedBottom(12.0f);
    axes_.draw(g, getLocalBounds(), 0, true, [&](juce::Graphics& sg) {
        drawWell(sg, area);
        drawFrequencyAxis(sg, plot);
        drawText(sg, "dBFS", {2.0f, 3.0f, plot.getX() - 6.0f, 15.0f}, kTextSmall, t.textDim, juce::Justification::centredRight);
        for (const int db : {-30, -60, -90}) {
            const float y = plot.getY() - plot.getHeight() * db / 90.0f;
            sg.setColour(t.displayGrid);
            sg.fillRect(plot.getX(), y, plot.getWidth(), 1.0f);
            drawText(sg, juce::String(db), {2.0f, y - 7.0f, plot.getX() - 7.0f, 14.0f}, kTextSmall, t.textDim, juce::Justification::centredRight);
        }
    });
    if (!smoothed_.empty()) {
        juce::Path p;
        bool visible = false;
        for (std::size_t b = 0; b < smoothed_.size(); ++b) {
            const float x = plot.getX() + plot.getWidth() * static_cast<float>(b) / static_cast<float>(smoothed_.size() - 1);
            const float y = plot.getBottom() - plot.getHeight() * juce::jlimit(0.0f, 1.0f, (smoothed_[b] + 90.0f) / 90.0f);
            visible = visible || smoothed_[b] > -90.0f;
            if (b == 0) p.startNewSubPath(x, y);
            else p.lineTo(x, y);
        }
        if (visible) {
            const auto colour = t.displayGrid.interpolatedWith(t.displayLine, 0.47f);
            g.setColour(colour);
            g.strokePath(p, juce::PathStrokeType(1.15f));
            p.lineTo(plot.getRight(), plot.getBottom());
            p.lineTo(plot.getX(), plot.getBottom());
            p.closeSubPath();
            g.setGradientFill({colour.withAlpha(0.53f), plot.getTopLeft(), colour.withAlpha(0.12f), plot.getBottomLeft(), false});
            g.fillPath(p);
        }
    }
    if (curve) {
        if (curve(responseHz_, responseDb_)) {
            g.setColour(t.sources.withAlpha(0.25f));
            g.fillRect(plot.getX(), plot.getCentreY(), plot.getWidth(), 1.0f);
            juce::Path p;
            for (std::size_t i = 0; i < responseHz_.size(); ++i) {
                const float x = plot.getX() + plot.getWidth() * static_cast<float>(i) / 159.0f;
                const float y = plot.getCentreY() - plot.getHeight() * 0.5f * juce::jlimit(-1.0f, 1.0f, responseDb_[i] / 24.0f);
                if (i == 0)
                    p.startNewSubPath(x, y);
                else
                    p.lineTo(x, y);
            }
            strokeGlow(g, p, t.sources.interpolatedWith(t.displayLine, 0.35f), 1.8f);
            drawText(g, "0 dB", plot.withY(plot.getCentreY() + 2.0f).withHeight(14.0f).reduced(3.0f, 0.0f), kTextSmall,
                     t.textDim, juce::Justification::centredRight);
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

// --- Source scope ------------------------------------------------------------------------------------

SourceScope::SourceScope(Context& ctx) : ctx_(ctx) {
    setTitle("Source output");
    setHelp("Output", "The modulator's output over the last seconds.");
    history_.assign(240, 0.0f);
}

bool SourceScope::tick() {
    const int i = Context::sourceIndex(source);
    const float v = i >= 0 ? ctx_.processor.engine().sourceValue(i) : 0.0f;
    history_[static_cast<std::size_t>(pos_)] = v;
    pos_ = (pos_ + 1) % static_cast<int>(history_.size());
    if (std::abs(v - last_) > 1e-4f) {
        last_ = v;
        moving_ = static_cast<int>(history_.size());
    }
    if (moving_ <= 0)
        return false;
    --moving_;
    return true;
}

void SourceScope::draw(juce::Graphics& g) {
    const Theme& t = theme();
    const auto area = getLocalBounds().toFloat();
    drawWell(g, area);
    const auto plot = area.reduced(8.0f);
    drawScopeGrid(g, plot);
    juce::Path p;
    const int n = static_cast<int>(history_.size());
    for (int i = 0; i < n; ++i) {
        const float v = history_[static_cast<std::size_t>((pos_ + i) % n)];
        const float x = plot.getX() + plot.getWidth() * static_cast<float>(i) / static_cast<float>(n - 1);
        const float y = plot.getCentreY() - juce::jlimit(-1.0f, 1.0f, v) * plot.getHeight() * 0.45f;
        if (i == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
    }
    strokeGlow(g, p, ctx_.sourceColour(source), 1.6f);
    drawText(g, Context::sourceName(source) + "  " + juce::String(last_, 2), area.reduced(10.0f, 6.0f).removeFromTop(14.0f), kTextSmall,
             t.textDim, juce::Justification::centredRight);
}

} // namespace zui
