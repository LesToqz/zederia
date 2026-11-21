#include "OutputMonitor.h"

#include "Audit.h"

namespace zui {

namespace {
void graticule(juce::Graphics& g, juce::Rectangle<float> plot, int divisions) {
    const auto& t = theme();
    g.setColour(t.displayGrid);
    for (int i = 1; i < divisions; ++i)
        g.drawVerticalLine(juce::roundToInt(plot.getX() + plot.getWidth() * i / divisions), plot.getY(), plot.getBottom());
    for (int i = 1; i < 4; ++i) {
        g.setColour(i == 2 ? t.lineStrong : t.displayGrid);
        g.drawHorizontalLine(juce::roundToInt(plot.getY() + plot.getHeight() * i / 4.0f), plot.getX(), plot.getRight());
    }
}
} // namespace

OutputScope::OutputScope(Context& ctx) : Button("Output scope"), ctx_(ctx) {
    setHelp("Output scope", "See the mono output waveform and spectrum. Capture a reference to compare the next edit against it.");
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

bool OutputScope::tick() {
    if ((frames_++ & 1u) != 0)
        return false;
    const double rate = ctx_.processor.currentSampleRate();
    if (rate <= 0.0)
        return false;
    const int capacity = static_cast<int>(scratch_.size());
    const int length = juce::jlimit(2, capacity, juce::roundToInt(rate * 0.012));
    const int read = ctx_.processor.readOutput(scratch_.data(), std::min(capacity, length * 2));
    if (read < length)
        return false;
    // Trigger on the most recent rising zero crossing that leaves a complete window. If none
    // exists (a long bass wave or DC), show the newest window on the same time scale.
    int start = read - length;
    for (int i = start; i > 0; --i) {
        if (scratch_[i - 1] <= 0.0f && scratch_[i] > 0.0f) {
            start = i;
            break;
        }
    }
    Trace next;
    for (int x = 0; x < Trace::columns; ++x) {
        const int first = start + x * length / Trace::columns;
        const int end = std::max(first + 1, start + (x + 1) * length / Trace::columns);
        const auto [low, high] = std::minmax_element(scratch_.begin() + first, scratch_.begin() + end);
        next.low[x] = *low;
        next.high[x] = *high;
        next.peak = std::max({next.peak, std::abs(*low), std::abs(*high)});
    }
    const double duration = 1000.0 * length / rate;
    if (std::abs(duration - spanMs_) < 0.1) {
        if (live_.low == next.low && live_.high == next.high)
            return false;
        // A sub-pixel signal still exists and can be captured. Its visible reading stays
        // below the display floor until the output falls silent or crosses -80 dBFS.
        if (hasSignal() && next.peak > 0.0f && std::max(live_.peak, next.peak) < 0.0001f) {
            live_ = next;
            return false;
        }
    }
    if (std::abs(duration - spanMs_) >= 0.1)
        referenceHeld_ = false;
    spanMs_ = duration;
    live_ = next;
    ++revision_;
    return true;
}

void OutputScope::capture() {
    if (!hasSignal())
        return;
    reference_ = live_;
    referenceHeld_ = true;
    ++revision_;
    repaint();
}

void OutputScope::clearReference() {
    referenceHeld_ = false;
    ++revision_;
    repaint();
}

juce::String OutputScope::peakText(bool reference) const {
    const float peak = reference ? reference_.peak : live_.peak;
    if (peak > 0.0f && peak < 0.0001f)
        return "< -80 dBFS";
    return peak > 0.0f ? juce::String(juce::Decibels::gainToDecibels(peak), 1) + " dBFS" : juce::String::fromUTF8("\xe2\x88\x92\xe2\x88\x9e dBFS");
}

void OutputScope::drawTraces(juce::Graphics& g, juce::Rectangle<float> plot, float gain) const {
    const juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(plot.getSmallestIntegerContainer());
    const auto draw = [&](const Trace& trace, juce::Colour ink, bool dashed) {
        if (trace.peak == 0.0f)
            return;
        const int columns = juce::jlimit(2, Trace::columns, juce::roundToInt(plot.getWidth()));
        const float dx = (plot.getWidth() - 1.0f) / static_cast<float>(columns - 1);
        const auto yOf = [&](float v) { return plot.getCentreY() - juce::jlimit(-1.0f, 1.0f, v * gain) * (plot.getHeight() - 1.0f) * 0.5f; };
        juce::Path centre, peaks;
        for (int col = 0; col < columns; ++col) {
            const int begin = col * Trace::columns / columns, end = (col + 1) * Trace::columns / columns;
            const float low = *std::min_element(trace.low.begin() + begin, trace.low.begin() + end);
            const float high = *std::max_element(trace.high.begin() + begin, trace.high.begin() + end);
            const float x = plot.getX() + dx * col;
            const float y = yOf(0.5f * (low + high));
            if (col == 0) centre.startNewSubPath(x, y); else centre.lineTo(x, y);
            if (yOf(low) - yOf(high) > 1.0f && (!dashed || col % 6 < 3)) {
                peaks.startNewSubPath(x, yOf(low));
                peaks.lineTo(x, yOf(high));
            }
        }
        g.setColour(ink.withMultipliedAlpha(0.6f));
        g.strokePath(peaks, juce::PathStrokeType(1.0f));
        g.setColour(ink);
        if (dashed) {
            constexpr float dash[]{3.0f, 3.0f};
            juce::Path outline;
            juce::PathStrokeType(1.25f).createDashedStroke(outline, centre, dash, 2);
            g.fillPath(outline);
        } else {
            g.strokePath(centre, juce::PathStrokeType(1.4f, juce::PathStrokeType::beveled));
        }
    };
    draw(live_, theme().displayLine, false);
    if (referenceHeld_)
        draw(reference_, theme().lamp, true);
}

void OutputScope::draw(juce::Graphics& g) {
    const auto& t = theme();
    const auto bounds = getLocalBounds().toFloat();
    drawWell(g, bounds);
    auto inner = bounds.reduced(7.0f, 4.0f);
    auto legend = inner.removeFromTop(14.0f);
    const auto icon = legend.removeFromRight(10.0f).withSizeKeepingCentre(7.0f, 7.0f);
    g.setColour(isMouseOver() ? t.lamp : t.textDim);
    g.drawLine(icon.getX(), icon.getBottom(), icon.getRight(), icon.getY(), 1.2f);
    g.fillRect(icon.withHeight(1.0f));
    g.fillRect(icon.withTrimmedLeft(6.0f));
    drawText(g, "OUTPUT", legend, kTextSmall, t.text, juce::Justification::centredLeft, true);
    const float peak = referenceHeld_ ? std::max(live_.peak, reference_.peak) : live_.peak;
    float gain = 1.0f;
    while (peak > 0.0f && gain < 16.0f && gain * peak <= 0.42f)
        gain *= 2.0f;
    if (gain > 1.0f)
        drawText(g, juce::String(static_cast<int>(gain)) + "x", legend.withTrimmedRight(5.0f), kTextSmall, t.textDim, juce::Justification::centredRight);
    inner.removeFromTop(3.0f);
    graticule(g, inner, 4);
    drawTraces(g, inner, gain);
    drawFocus(g, bounds, 1.0f);
}

OutputMonitor::OutputMonitor(Context& ctx, OutputScope& scope) : scope_(scope), spectrum_(ctx) {
    setTitle("Output monitor");
    setOpaque(true);
    setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    markOverlay(*this);
    close.setTitle("Close output monitor");
    close.setTooltip("Close output monitor");
    zoom_.toggleRole = true;
    zoom_.setHelp("4x zoom", "Magnify both time-domain traces vertically. The meter and spectrum keep their original scale.");
    zoom_.onClick = [this] { zoom_.setToggled(!zoom_.toggled()); repaint(); };
    capture_.setHelp("Capture reference", "Keep the current mono output trace for a visual comparison while playing or switching A/B.");
    capture_.onClick = [this] { scope_.capture(); refresh(); };
    clear_.setTitle("Clear reference");
    clear_.onClick = [this] { scope_.clearReference(); refresh(); };
    for (juce::Component* c : std::initializer_list<juce::Component*>{&spectrum_, &capture_, &clear_, &zoom_, &close})
        addAndMakeVisible(c);
}

void OutputMonitor::refresh() {
    capture_.setEnabled(scope_.hasSignal());
    clear_.setEnabled(scope_.hasReference());
    seen_ = scope_.revision();
    repaint();
}

void OutputMonitor::visibilityChanged() {
    if (isVisible()) refresh();
}

bool OutputMonitor::tick() {
    if (seen_ == scope_.revision()) return false;
    if (clear_.isEnabled() != scope_.hasReference())
        repaint(); // A change of sample span can invalidate the reference legend.
    capture_.setEnabled(scope_.hasSignal());
    clear_.setEnabled(scope_.hasReference());
    seen_ = scope_.revision();
    return true;
}

void OutputMonitor::resized() {
    auto body = getLocalBounds().reduced(14, 10);
    auto head = body.removeFromTop(28);
    close.setBounds(head.removeFromRight(26));
    auto foot = body.removeFromBottom(28);
    capture_.setBounds(foot.removeFromLeft(150));
    foot.removeFromLeft(4);
    clear_.setBounds(foot.removeFromLeft(50));
    zoom_.setBounds(foot.removeFromRight(80));
    body.reduce(0, 8);
    traceArea_ = body.removeFromLeft(body.getWidth() * 53 / 100);
    body.removeFromLeft(12);
    spectrumArea_ = body;
    spectrum_.setBounds(spectrumArea_.withTrimmedTop(22));
}

void OutputMonitor::draw(juce::Graphics& g) {
    const auto& t = theme();
    drawPanel(g, getLocalBounds().toFloat(), t.bg);
    drawText(g, "Output monitor", {14.0f, 10.0f, 200.0f, 28.0f}, kTextLarge, t.text, juce::Justification::centredLeft, true);
    auto top = traceArea_.toFloat().removeFromTop(20.0f);
    drawText(g, "Waveform", top.removeFromLeft(92.0f), kTextBody, t.text, juce::Justification::centredLeft, true);
    drawText(g, "Mono peak   " + scope_.peakText(false), top, kTextSmall, t.textDim, juce::Justification::centredRight);
    drawText(g, "Output spectrum", spectrumArea_.toFloat().removeFromTop(20.0f), kTextBody, t.text, juce::Justification::centredLeft, true);
    auto glass = traceArea_.toFloat().withTrimmedTop(22.0f);
    drawWell(g, glass);
    auto plot = glass.reduced(9.0f, 6.0f);
    auto units = plot.removeFromBottom(16.0f);
    const float gain = zoom_.toggled() ? 4.0f : 1.0f;
    graticule(g, plot, 6);
    scope_.drawTraces(g, plot, gain);
    drawText(g, gain == 1.0f ? juce::String::fromUTF8("\xc2\xb1" "1 FS") : juce::String::fromUTF8("\xc2\xb1" "0.25 FS"), units,
             kTextSmall, t.textDim);
    drawText(g, juce::String(scope_.spanMs(), 0) + " ms", units, kTextSmall, t.textDim, juce::Justification::centredRight);
    if (scope_.hasReference()) {
        const auto foot = juce::Rectangle<int>(clear_.getRight() + 8, clear_.getY(), zoom_.getX() - clear_.getRight() - 16, clear_.getHeight()).toFloat();
        drawText(g, "Reference   " + scope_.peakText(true), foot, kTextBody, t.lamp);
    }
}

} // namespace zui
