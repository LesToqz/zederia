#include "PlayPage.h"

#include "zederia/params/ValueText.h"

namespace zui {

namespace {

const zed::Registry& reg() { return zed::Registry::get(); }
int idx(const char* id) { return reg().indexOf(id); }

} // namespace

// --- The arpeggiator's step lane ------------------------------------------------------------------------

class StepLane final : public Widget {
public:
    StepLane(Context& ctx, PlayPage& page) : ctx_(ctx), page_(page) {
        setWantsKeyboardFocus(true);
        setTitle("Step lane");
        setHelp("Step lane", "Each step: click the top cell for on, tie or rest; drag the bar for velocity; drag the lower cells for transposition and ratchets. Arrow keys move, Space changes the state.");
        family = Family::Play;
    }
    zed::ArpPattern pattern() const {
        zed::ArpPattern p;
        ctx_.processor.readDocument([&](const zed::Patch& d) { p = d.arpPatterns[static_cast<std::size_t>(page_.patternSlot())]; });
        return p;
    }
    int steps() const { return static_cast<int>(ctx_.processor.plainValue(idx("arp.steps"))); }
    bool tick() override {
        const int s = steps(), slot = page_.patternSlot();
        if (s != shownSteps_ || slot != shownSlot_) {
            shownSteps_ = s;
            shownSlot_ = slot;
            return true;
        }
        return false;
    }
    struct Rows {
        juce::Rectangle<float> state, velocity, transpose, ratchet;
    };
    Rows rows() const {
        auto area = getLocalBounds().toFloat().reduced(4.0f);
        area.removeFromLeft(70.0f);
        Rows r;
        r.state = area.removeFromTop(22.0f);
        area.removeFromTop(4.0f);
        r.ratchet = area.removeFromBottom(20.0f);
        area.removeFromBottom(4.0f);
        r.transpose = area.removeFromBottom(20.0f);
        area.removeFromBottom(4.0f);
        r.velocity = area;
        return r;
    }
    void draw(juce::Graphics& g) override {
        const Theme& t = theme();
        drawWell(g, getLocalBounds().toFloat());
        const auto r = rows();
        const auto labels = getLocalBounds().toFloat().reduced(8.0f, 4.0f).withWidth(62.0f);
        drawText(g, "Step", labels.withY(r.state.getY()).withHeight(r.state.getHeight()), kTextSmall, t.textDim);
        drawText(g, "Velocity", labels.withY(r.velocity.getCentreY() - 7.0f).withHeight(14.0f), kTextSmall, t.textDim);
        drawText(g, "Transpose", labels.withY(r.transpose.getY()).withHeight(r.transpose.getHeight()), kTextSmall, t.textDim);
        drawText(g, "Ratchet", labels.withY(r.ratchet.getY()).withHeight(r.ratchet.getHeight()), kTextSmall, t.textDim);
        const auto p = editing_ ? live_ : pattern();
        const int n = juce::jmax(1, steps());
        const float w = r.state.getWidth() / static_cast<float>(n);
        for (int i = 0; i < n; ++i) {
            const auto s = p.step(i);
            const float x = r.state.getX() + w * static_cast<float>(i);
            const bool focused = i == focusStep_ && hasKeyboardFocus(false);
            auto cell = juce::Rectangle<float>(x + 1.0f, r.state.getY(), w - 2.0f, r.state.getHeight());
            g.setColour(s.state == zed::StepState::On ? t.play : s.state == zed::StepState::Tie ? t.play.withAlpha(0.45f) : t.sunken);
            g.fillRoundedRectangle(cell, 2.0f);
            if (focused) {
                g.setColour(t.focus);
                g.drawRoundedRectangle(cell, 2.0f, 1.5f);
            }
            if (w > 16.0f)
                drawText(g, s.state == zed::StepState::On ? juce::String(i + 1) : s.state == zed::StepState::Tie ? "tie" : "-", cell, kTextSmall,
                         s.state == zed::StepState::On ? t.bg : t.textDim, juce::Justification::centred);
            const auto bar = juce::Rectangle<float>(x + 2.0f, r.velocity.getBottom() - r.velocity.getHeight() * static_cast<float>(s.velocity), w - 4.0f,
                                                    r.velocity.getHeight() * static_cast<float>(s.velocity));
            g.setColour(t.play.withAlpha(s.state == zed::StepState::Rest ? 0.25f : 0.8f));
            g.fillRect(bar);
            const auto tcell = juce::Rectangle<float>(x + 1.0f, r.transpose.getY(), w - 2.0f, r.transpose.getHeight());
            g.setColour(t.raised);
            g.fillRect(tcell);
            if (w > 16.0f)
                drawText(g, s.transpose == 0 ? juce::String("0") : (s.transpose > 0 ? "+" : "") + juce::String(s.transpose), tcell, kTextSmall,
                         s.transpose == 0 ? t.textDim : t.text, juce::Justification::centred);
            const auto rcell = juce::Rectangle<float>(x + 1.0f, r.ratchet.getY(), w - 2.0f, r.ratchet.getHeight());
            g.setColour(t.raised);
            g.fillRect(rcell);
            if (w > 16.0f)
                drawText(g, juce::String(s.ratchet) + "x", rcell, kTextSmall, s.ratchet > 1 ? t.text : t.textDim, juce::Justification::centred);
        }
        drawFocus(g, getLocalBounds().toFloat());
    }
    int stepAt(float x) const {
        const auto r = rows();
        return juce::jlimit(0, juce::jmax(1, steps()) - 1, static_cast<int>((x - r.state.getX()) / r.state.getWidth() * static_cast<float>(juce::jmax(1, steps()))));
    }
    static void ensure(zed::ArpPattern& p, int i) {
        while (static_cast<int>(p.steps.size()) <= i && p.steps.size() < static_cast<std::size_t>(zed::kMaxArpSteps))
            p.steps.push_back({});
    }
    void mouseDown(const juce::MouseEvent& e) override {
        grabKeyboardFocus();
        live_ = pattern();
        editing_ = true;
        const auto r = rows();
        const int i = stepAt(e.position.x);
        focusStep_ = i;
        ensure(live_, i);
        auto& s = live_.steps[static_cast<std::size_t>(i)];
        if (r.state.contains(e.position)) {
            s.state = s.state == zed::StepState::On ? zed::StepState::Tie : s.state == zed::StepState::Tie ? zed::StepState::Rest : zed::StepState::On;
            region_ = 0;
        } else if (r.ratchet.contains(e.position)) {
            s.ratchet = s.ratchet % 4 + 1;
            region_ = 3;
        } else if (r.transpose.contains(e.position)) {
            region_ = 2;
            startTranspose_ = s.transpose;
        } else {
            region_ = 1;
        }
        mouseDrag(e);
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (!editing_)
            return;
        const auto r = rows();
        const int i = stepAt(e.position.x);
        ensure(live_, i);
        auto& s = live_.steps[static_cast<std::size_t>(i)];
        if (region_ == 1)
            s.velocity = juce::jlimit(0.0, 1.0, static_cast<double>((r.velocity.getBottom() - e.position.y) / r.velocity.getHeight()));
        else if (region_ == 2)
            s.transpose = juce::jlimit(-24, 24, startTranspose_ - e.getDistanceFromDragStartY() / 6);
        ctx_.processor.setArpPatternLive(page_.patternSlot(), live_);
        repaint();
    }
    void mouseUp(const juce::MouseEvent&) override {
        if (!editing_)
            return;
        editing_ = false;
        static const char* names[] = {"Step state", "Step velocity", "Step transposition", "Step ratchet"};
        ctx_.session.commit(juce::String(names[region_]) + ", pattern " + juce::String(page_.patternSlot() + 1));
    }
    bool keyPressed(const juce::KeyPress& key) override {
        const int n = juce::jmax(1, steps());
        if (key == juce::KeyPress::rightKey) { focusStep_ = (focusStep_ + 1) % n; repaint(); return true; }
        if (key == juce::KeyPress::leftKey) { focusStep_ = (focusStep_ + n - 1) % n; repaint(); return true; }
        if (key == juce::KeyPress::spaceKey || key == juce::KeyPress::upKey || key == juce::KeyPress::downKey) {
            auto p = pattern();
            ensure(p, focusStep_);
            auto& s = p.steps[static_cast<std::size_t>(focusStep_)];
            if (key == juce::KeyPress::spaceKey)
                s.state = s.state == zed::StepState::On ? zed::StepState::Tie : s.state == zed::StepState::Tie ? zed::StepState::Rest : zed::StepState::On;
            else
                s.velocity = juce::jlimit(0.0, 1.0, s.velocity + (key == juce::KeyPress::upKey ? 0.1 : -0.1));
            ctx_.session.edit("Step " + juce::String(focusStep_ + 1), [&](zed::Patch& d) {
                d.arpPatterns[static_cast<std::size_t>(page_.patternSlot())] = p;
                return true;
            });
            repaint();
            return true;
        }
        return false;
    }

private:
    Context& ctx_;
    PlayPage& page_;
    zed::ArpPattern live_;
    bool editing_ = false;
    int region_ = 0, startTranspose_ = 0, focusStep_ = 0, shownSteps_ = -1, shownSlot_ = -1;
};

// --- Chord memory ----------------------------------------------------------------------------------------

class ChordEditor final : public Widget {
public:
    explicit ChordEditor(Context& ctx) : ctx_(ctx) {
        setWantsKeyboardFocus(true);
        setTitle("Chord notes");
        setHelp("Chord notes", "The chord one key plays: click semitones above the key (0 is the key itself). Arrow keys move, Space toggles.");
        family = Family::Play;
    }
    std::vector<int> notes() const {
        std::vector<int> n;
        ctx_.processor.readDocument([&](const zed::Patch& p) { n = p.chordNotes; });
        return n;
    }
    void draw(juce::Graphics& g) override {
        const Theme& t = theme();
        const auto area = getLocalBounds().toFloat();
        drawWell(g, area);
        const auto n = notes();
        const auto inner = area.reduced(6.0f, 4.0f);
        const float w = inner.getWidth() / 25.0f;
        static const bool black[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
        for (int s = 0; s <= 24; ++s) {
            const auto cell = juce::Rectangle<float>(inner.getX() + w * static_cast<float>(s), inner.getY(), w - 1.0f, inner.getHeight() - 14.0f);
            const bool on = std::find(n.begin(), n.end(), s) != n.end();
            g.setColour(on ? t.play : black[s % 12] ? t.sunken : t.raised);
            g.fillRoundedRectangle(cell, 2.0f);
            if (s == focus_ && hasKeyboardFocus(false)) {
                g.setColour(t.focus);
                g.drawRoundedRectangle(cell, 2.0f, 1.5f);
            }
            if (s % 12 == 0 || s == 4 || s == 7 || s == 16 || s == 19)
                drawText(g, juce::String(s), juce::Rectangle<float>(cell.getX() - 4.0f, inner.getBottom() - 13.0f, w + 8.0f, 13.0f), kTextSmall, t.textDim,
                         juce::Justification::centred);
        }
    }
    void toggle(int s) {
        auto n = notes();
        const auto it = std::find(n.begin(), n.end(), s);
        if (it != n.end()) {
            if (n.size() > 1)
                n.erase(it);
        } else if (n.size() < static_cast<std::size_t>(zed::kMaxChordNotes)) {
            n.push_back(s);
            std::sort(n.begin(), n.end());
        }
        ctx_.session.edit("Chord notes", [&](zed::Patch& p) {
            p.chordNotes = n;
            return true;
        });
        repaint();
    }
    void mouseDown(const juce::MouseEvent& e) override {
        grabKeyboardFocus();
        const auto inner = getLocalBounds().toFloat().reduced(6.0f, 4.0f);
        focus_ = juce::jlimit(0, 24, static_cast<int>((e.position.x - inner.getX()) / (inner.getWidth() / 25.0f)));
        toggle(focus_);
    }
    bool keyPressed(const juce::KeyPress& key) override {
        if (key == juce::KeyPress::rightKey) { focus_ = juce::jmin(24, focus_ + 1); repaint(); return true; }
        if (key == juce::KeyPress::leftKey) { focus_ = juce::jmax(0, focus_ - 1); repaint(); return true; }
        if (key == juce::KeyPress::spaceKey) { toggle(focus_); return true; }
        return false;
    }

private:
    Context& ctx_;
    int focus_ = 0;
};

// --- Piano roll --------------------------------------------------------------------------------------------

class PianoRoll final : public Widget {
public:
    PianoRoll(Context& ctx, PlayPage& page) : ctx_(ctx), page_(page) {
        setWantsKeyboardFocus(true);
        setTitle("Piano roll");
        setHelp("Piano roll", "Double-click to add a note; drag to move, drag the right edge for length, drag in the lower strip for velocity. Delete removes; the Quantize menu snaps starts.");
        family = Family::Play;
    }
    double grid = 0.25; // quarter notes (a sixteenth)
    zed::Clip clip() const { return editing_ ? live_ : ctx_.processor.clip(page_.clipSlot()); }
    std::set<int> selected;

    juce::Rectangle<float> notesArea() const { return getLocalBounds().toFloat().reduced(4.0f).withTrimmedLeft(30.0f).withTrimmedBottom(46.0f); }
    juce::Rectangle<float> velocityArea() const {
        return getLocalBounds().toFloat().reduced(4.0f).withTrimmedLeft(30.0f).removeFromBottom(40.0f);
    }
    void range(const zed::Clip& c, int& low, int& high) const {
        low = 48;
        high = 72;
        for (const auto& n : c.notes) {
            low = std::min(low, n.key - 2);
            high = std::max(high, n.key + 2);
        }
        if (high - low < 24)
            high = low + 24;
    }
    bool tick() override {
        const int slot = page_.clipSlot();
        if (slot != shownSlot_) {
            shownSlot_ = slot;
            selected.clear();
            return true;
        }
        return false;
    }
    void draw(juce::Graphics& g) override {
        const Theme& t = theme();
        drawWell(g, getLocalBounds().toFloat());
        const auto c = clip();
        const auto area = notesArea();
        int low = 0, high = 0;
        range(c, low, high);
        const double beats = std::max(1.0, c.lengthBeats());
        const float rowH = area.getHeight() / static_cast<float>(high - low + 1);
        for (int k = low; k <= high; ++k) { // keyboard lanes
            const int pc = ((k % 12) + 12) % 12;
            const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
            const float y = area.getBottom() - rowH * static_cast<float>(k - low + 1);
            if (black) {
                g.setColour(t.sunken);
                g.fillRect(area.getX(), y, area.getWidth(), rowH);
            }
            if (pc == 0)
                drawText(g, str(zed::noteName(k)), juce::Rectangle<float>(4.0f, y + rowH - 13.0f, 30.0f, 13.0f), kTextSmall, t.textDim);
        }
        for (double b = 0.0; b <= beats + 1e-9; b += 1.0) {
            const float x = area.getX() + area.getWidth() * static_cast<float>(b / beats);
            g.setColour(std::fmod(b, c.beatsPerBar) < 1e-9 ? t.lineStrong : t.displayGrid);
            g.fillRect(x, area.getY(), 1.0f, area.getHeight() + 46.0f);
        }
        const auto vel = velocityArea();
        for (int i = 0; i < static_cast<int>(c.notes.size()); ++i) {
            const auto& n = c.notes[static_cast<std::size_t>(i)];
            const auto box = noteBox(n, area, low, rowH, beats);
            const bool sel = selected.count(i) > 0;
            g.setColour((sel ? t.focus : t.play).withAlpha(0.45f + 0.55f * static_cast<float>(n.velocity)));
            g.fillRoundedRectangle(box, 2.0f);
            g.setColour(sel ? t.focus : t.play);
            g.drawRoundedRectangle(box, 2.0f, 1.0f);
            const float x = box.getX();
            g.setColour(sel ? t.focus : t.play);
            g.fillRect(juce::Rectangle<float>(x, vel.getBottom() - vel.getHeight() * static_cast<float>(n.velocity), 3.0f, vel.getHeight() * static_cast<float>(n.velocity)));
        }
        drawText(g, juce::String(static_cast<int>(c.notes.size())) + " notes, " + juce::String(c.bars, 2) + " bars" + (c.name.empty() ? "" : "  " + str(c.name)),
                 getLocalBounds().toFloat().reduced(40.0f, 6.0f).removeFromTop(14.0f), kTextSmall, t.textDim, juce::Justification::centredRight);
        if (c.notes.empty())
            drawText(g, "Empty clip: double-click to add notes, or drop a MIDI file here.", area, kTextBody, t.textDim, juce::Justification::centred);
        drawFocus(g, getLocalBounds().toFloat());
    }
    juce::Rectangle<float> noteBox(const zed::ClipNote& n, juce::Rectangle<float> area, int low, float rowH, double beats) const {
        const float x0 = area.getX() + area.getWidth() * static_cast<float>(n.start / beats);
        const float x1 = area.getX() + area.getWidth() * static_cast<float>((n.start + n.length) / beats);
        const float y = area.getBottom() - rowH * static_cast<float>(n.key - low + 1);
        return {x0, y + 1.0f, std::max(3.0f, x1 - x0 - 1.0f), rowH - 2.0f};
    }
    int noteAt(juce::Point<float> p, bool& edge) const {
        const auto c = clip();
        const auto area = notesArea();
        int low = 0, high = 0;
        range(c, low, high);
        const float rowH = area.getHeight() / static_cast<float>(high - low + 1);
        const double beats = std::max(1.0, c.lengthBeats());
        for (int i = static_cast<int>(c.notes.size()) - 1; i >= 0; --i) {
            const auto box = noteBox(c.notes[static_cast<std::size_t>(i)], area, low, rowH, beats);
            if (box.expanded(0.0f, 1.0f).contains(p)) {
                edge = p.x > box.getRight() - 5.0f;
                return i;
            }
        }
        return -1;
    }
    void mouseDown(const juce::MouseEvent& e) override {
        grabKeyboardFocus();
        live_ = ctx_.processor.clip(page_.clipSlot());
        origin_ = live_;
        editing_ = true;
        if (velocityArea().contains(e.position)) {
            mode_ = Mode::Velocity;
            mouseDrag(e);
            return;
        }
        bool edge = false;
        const int i = noteAt(e.position, edge);
        if (i < 0) {
            if (!e.mods.isShiftDown())
                selected.clear();
            mode_ = Mode::None;
            repaint();
            return;
        }
        if (!selected.count(i)) {
            if (!e.mods.isShiftDown())
                selected.clear();
            selected.insert(i);
        }
        mode_ = edge ? Mode::Length : Mode::Move;
        repaint();
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (!editing_ || mode_ == Mode::None)
            return;
        const auto area = notesArea();
        int low = 0, high = 0;
        range(origin_, low, high);
        const float rowH = area.getHeight() / static_cast<float>(high - low + 1);
        const double beats = std::max(1.0, origin_.lengthBeats());
        const double dBeats = std::round(e.getDistanceFromDragStartX() / area.getWidth() * beats / grid) * grid;
        const int dKeys = -static_cast<int>(std::round(e.getDistanceFromDragStartY() / rowH));
        live_ = origin_;
        if (mode_ == Mode::Velocity) {
            const auto vel = velocityArea();
            const double beat = (e.position.x - area.getX()) / area.getWidth() * beats;
            const double v = juce::jlimit(0.01, 1.0, static_cast<double>((vel.getBottom() - e.position.y) / vel.getHeight()));
            for (auto& n : live_.notes)
                if (beat >= n.start && beat < n.start + std::max(n.length, grid))
                    n.velocity = v;
            origin_ = live_; // velocity painting accumulates
        } else {
            for (const int i : selected) {
                if (i >= static_cast<int>(live_.notes.size()))
                    continue;
                auto& n = live_.notes[static_cast<std::size_t>(i)];
                const auto& o = origin_.notes[static_cast<std::size_t>(i)];
                if (mode_ == Mode::Move) {
                    n.start = juce::jlimit(0.0, std::max(0.0, beats - o.length), o.start + dBeats);
                    n.key = juce::jlimit(0, 127, o.key + dKeys);
                } else {
                    n.length = juce::jlimit(grid, beats - o.start, o.length + dBeats);
                }
            }
        }
        ctx_.processor.setClipLive(page_.clipSlot(), live_);
        repaint();
    }
    void mouseUp(const juce::MouseEvent&) override {
        if (!editing_)
            return;
        editing_ = false;
        if (mode_ != Mode::None && !(live_ == ctx_.processor.clip(page_.clipSlot()) && false))
            ctx_.session.commit(mode_ == Mode::Move ? "Move notes" : mode_ == Mode::Length ? "Note length" : "Note velocity");
        mode_ = Mode::None;
    }
    void mouseDoubleClick(const juce::MouseEvent& e) override {
        bool edge = false;
        const int hit = noteAt(e.position, edge);
        auto c = ctx_.processor.clip(page_.clipSlot());
        if (hit >= 0) {
            c.notes.erase(c.notes.begin() + hit);
            selected.clear();
            commitClip(c, "Delete note");
            return;
        }
        const auto area = notesArea();
        if (!area.contains(e.position))
            return;
        int low = 0, high = 0;
        range(c, low, high);
        const float rowH = area.getHeight() / static_cast<float>(high - low + 1);
        const double beats = std::max(1.0, c.lengthBeats());
        zed::ClipNote n;
        n.start = std::floor((e.position.x - area.getX()) / area.getWidth() * beats / grid) * grid;
        n.key = juce::jlimit(0, 127, low + static_cast<int>((area.getBottom() - e.position.y) / rowH));
        n.length = std::max(grid, 0.5);
        n.velocity = 0.8;
        c.notes.push_back(n);
        commitClip(c, "Add note " + str(zed::noteName(n.key)));
    }
    bool keyPressed(const juce::KeyPress& key) override {
        if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) && !selected.empty()) {
            auto c = ctx_.processor.clip(page_.clipSlot());
            std::vector<zed::ClipNote> kept;
            for (int i = 0; i < static_cast<int>(c.notes.size()); ++i)
                if (!selected.count(i))
                    kept.push_back(c.notes[static_cast<std::size_t>(i)]);
            c.notes = kept;
            selected.clear();
            commitClip(c, "Delete notes");
            return true;
        }
        if (key == juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0)) {
            selected.clear();
            for (int i = 0; i < static_cast<int>(clip().notes.size()); ++i)
                selected.insert(i);
            repaint();
            return true;
        }
        return false;
    }
    void quantize(double step) {
        auto c = ctx_.processor.clip(page_.clipSlot());
        for (int i = 0; i < static_cast<int>(c.notes.size()); ++i)
            if (selected.empty() || selected.count(i))
                c.notes[static_cast<std::size_t>(i)].start = std::round(c.notes[static_cast<std::size_t>(i)].start / step) * step;
        commitClip(c, "Quantize");
    }
    void commitClip(const zed::Clip& c, const juce::String& label) {
        const int slot = page_.clipSlot();
        ctx_.session.edit(label, [&](zed::Patch& p) {
            p.clips[static_cast<std::size_t>(slot)] = c;
            return true;
        });
        repaint();
    }

private:
    enum class Mode { None, Move, Length, Velocity };
    Context& ctx_;
    PlayPage& page_;
    zed::Clip live_, origin_;
    bool editing_ = false;
    Mode mode_ = Mode::None;
    int shownSlot_ = -1;
};

// --- The page ----------------------------------------------------------------------------------------

namespace {
class Ticker final : public Widget {
public:
    explicit Ticker(std::function<bool()> f) : f_(std::move(f)) {
        setInterceptsMouseClicks(false, false);
        setAccessible(false);
    }
    bool tick() override { return f_(); }
    void draw(juce::Graphics&) override {}
    bool hasVisibleLabel() const override { return false; }

private:
    std::function<bool()> f_;
};

class LayoutCard final : public Card {
public:
    LayoutCard(juce::String title, Family f, std::function<void(juce::Rectangle<int>)> layout) : Card(std::move(title), f), layout_(std::move(layout)) {}
    void layoutBody(juce::Rectangle<int> body) override {
        if (layout_)
            layout_(body);
    }

private:
    std::function<void(juce::Rectangle<int>)> layout_;
};
} // namespace

PlayPage::PlayPage(Context& ctx) : Page(ctx) {
    setTitle("PLAY");
    arp_ = std::make_unique<LayoutCard>("Arpeggiator", Family::Play, [this](juce::Rectangle<int> body) {
        const int used = flow(body, arpItems_, kKnobH, 6);
        body.removeFromTop(used + 8);
        lane_->setBounds(body);
    });
    chords_ = std::make_unique<LayoutCard>("Chords and scale", Family::Play, [this](juce::Rectangle<int> body) {
        chordEditor_->setBounds(body.removeFromTop(64));
        body.removeFromTop(8);
        flow(body, chordItems_, kKnobH, 6);
    });
    clips_ = std::make_unique<LayoutCard>("Clip player", Family::Play, [this](juce::Rectangle<int> body) {
        clipTabs_->setBounds(body.removeFromTop(24).withWidth(360));
        body.removeFromTop(6);
        const int used = flow(body.removeFromLeft(270), clipItems_, kChoiceH, 6);
        juce::ignoreUnused(used);
        body.removeFromLeft(8);
        roll_->setBounds(body);
    });
    for (auto* card : {arp_.get(), chords_.get(), clips_.get()})
        addAndMakeVisible(*card);
    arp_->moduleId = "arp";
    chords_->moduleId = "chord";
    auto& arpOn = parts_.add<Toggle>(*arp_, ctx, "arp.on", "Arpeggiator on");
    arpOn.power = true;
    arpOn.family = Family::Play;
    arp_->powerSwitch = &arpOn;
    const auto play = [](auto& w) -> auto& {
        w.family = Family::Play;
        return w;
    };
    auto& pattern = play(parts_.add<NumberBox>(*arp_, ctx, "arp.pattern", "Pattern"));
    auto& copyPattern = parts_.add<Button>(*arp_, "Copy", Glyph::Copy);
    auto& pastePattern = parts_.add<Button>(*arp_, "Paste", Glyph::Paste);
    for (Button* b : {&copyPattern, &pastePattern})
        b->textSize = kTextSmall;
    copyPattern.setHelp("Copy pattern", "Copy this step pattern.");
    pastePattern.setHelp("Paste pattern", "Paste a copied step pattern into this slot.");
    copyPattern.onClick = [this] { ctx_.session.copyModule("arp_pattern" + juce::String(patternSlot() + 1)); };
    pastePattern.onClick = [this] {
        juce::String error;
        if (!ctx_.session.pasteModule("arp_pattern" + juce::String(patternSlot() + 1), error) && ctx_.notify)
            ctx_.notify(error);
    };
    arpItems_ = {{&play(parts_.add<ChoiceBox>(*arp_, ctx, "arp.mode", "Mode")), 108, kChoiceH}, {&play(parts_.add<NumberBox>(*arp_, ctx, "arp.octaves", "Octaves")), 58, kChoiceH},
                 {&play(parts_.add<ChoiceBox>(*arp_, ctx, "arp.rate", "Rate")), 80, kChoiceH}, {&play(parts_.add<Knob>(*arp_, ctx, "arp.gate", "Gate")), kKnobW},
                 {&play(parts_.add<Knob>(*arp_, ctx, "arp.swing", "Swing")), kKnobW}, {&play(parts_.add<Toggle>(*arp_, ctx, "arp.latch", "Latch")), 64, 22},
                 {&play(parts_.add<Toggle>(*arp_, ctx, "arp.restart", "Restart")), 80, 22}, {&play(parts_.add<NumberBox>(*arp_, ctx, "arp.steps", "Steps")), 56, kChoiceH},
                 {&pattern, 58, kChoiceH}, {&copyPattern, 64, 22}, {&pastePattern, 68, 22}};
    lane_ = std::make_unique<StepLane>(ctx, *this);
    arp_->addAndMakeVisible(*lane_);

    chordEditor_ = std::make_unique<ChordEditor>(ctx);
    chords_->addAndMakeVisible(*chordEditor_);
    auto& memory = play(parts_.add<Toggle>(*chords_, ctx, "chord.memory", "Chord memory"));
    auto& learn = parts_.add<Button>(*chords_, "Learn held keys");
    learn.textSize = kTextSmall;
    learn.setHelp("Learn held keys", "Hold a chord on the keyboard, then click: it becomes the chord memory (relative to its lowest key).");
    learn.onClick = [this] {
        std::vector<int> held;
        for (int k = 0; k < 128; ++k)
            if (ctx_.processor.keyboardState().isNoteOnForChannels(0xffff, k))
                held.push_back(k);
        if (held.empty()) {
            if (ctx_.notify)
                ctx_.notify("Hold some keys on the keyboard first.");
            return;
        }
        std::vector<int> chord;
        for (const int k : held)
            if (chord.size() < static_cast<std::size_t>(zed::kMaxChordNotes))
                chord.push_back(k - held.front());
        ctx_.session.edit("Learn chord", [&](zed::Patch& p) { p.chordNotes = chord; return true; });
        chordEditor_->repaint();
    };
    auto& lockMode = play(parts_.add<Segmented>(*chords_, ctx, "chord.lock_mode", "Lock mode"));
    lockMode.names = {"Snap", "Filter"};
    chordItems_ = {{&memory, 120, 22}, {&learn, 120, 22}, {&play(parts_.add<Toggle>(*chords_, ctx, "chord.scale_lock", "Scale lock")), 104, 22, true},
                   {&play(parts_.add<ChoiceBox>(*chords_, ctx, "chord.key", "Key")), 64, kChoiceH}, {&play(parts_.add<ChoiceBox>(*chords_, ctx, "chord.scale", "Scale")), 130, kChoiceH},
                   {&lockMode, 110, kChoiceH}, {&play(parts_.add<Knob>(*chords_, ctx, "chord.strum", "Strum")), kKnobW, 0, true},
                   {&play(parts_.add<ChoiceBox>(*chords_, ctx, "chord.strum_direction", "Direction")), 110, kChoiceH}};

    auto& clipOn = parts_.add<Toggle>(*clips_, ctx, "clip.on", "Clip player on");
    clipOn.power = true;
    clipOn.family = Family::Play;
    clips_->powerSwitch = &clipOn;
    auto& slots = play(parts_.add<Segmented>(*clips_, ctx, "clip.slot", "Clip slot"));
    slots.names = {"1", "2", "3", "4", "5", "6", "7", "8"};
    slots.showLabel = false;
    slots.labelledBy = "Clip player";
    clipTabs_ = &slots;
    auto& trigger = play(parts_.add<Segmented>(*clips_, ctx, "clip.trigger", "Trigger"));
    trigger.names = {"Transport", "Key"};
    auto& import = parts_.add<Button>(*clips_, "Import MIDI", Glyph::Folder);
    auto& exportButton = parts_.add<Button>(*clips_, "Export MIDI", Glyph::Save);
    auto& drag = parts_.add<Button>(*clips_, "Drag to host", Glyph::Grip);
    auto& quantize = parts_.add<Button>(*clips_, "Quantize");
    auto& clearClip = parts_.add<Button>(*clips_, "Clear clip", Glyph::Trash);
    for (Button* b : {&import, &exportButton, &drag, &quantize, &clearClip})
        b->textSize = kTextSmall + 0.5f;
    import.setHelp("Import MIDI", "Put a Standard MIDI File into this clip slot (or drop one on the page).");
    exportButton.setHelp("Export MIDI", "Save this clip as a Standard MIDI File.");
    drag.setHelp("Drag to host", "Drag from here into your host's arrangement to drop the clip as MIDI.");
    quantize.setHelp("Quantize", "Snap note starts (the selected notes, or all) to a grid.");
    import.onClick = [this] {
        auto chooser = std::make_shared<juce::FileChooser>("Import a MIDI file", ctx_.prefs.userContentFolder(), "*.mid;*.midi");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, chooser](const juce::FileChooser& fc) {
            juce::String error;
            if (fc.getResult().existsAsFile() && !importClipFile(fc.getResult(), error) && ctx_.notify)
                ctx_.notify(error);
        });
    };
    exportButton.onClick = [this] {
        auto chooser = std::make_shared<juce::FileChooser>("Export the clip", ctx_.prefs.userContentFolder().getChildFile("Clips").getChildFile("Clip.mid"), "*.mid");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this, chooser](const juce::FileChooser& fc) {
            if (fc.getResult() != juce::File())
                exportClipFile(fc.getResult());
        });
    };
    drag.onClick = [this] {
        if (ctx_.notify)
            ctx_.notify("Press on Drag to host and drag into the host's arrangement.");
    };
    drag.addMouseListener(this, false);
    quantize.onClick = [this, &quantize] {
        juce::PopupMenu menu;
        for (const auto& [name, step] : {std::pair{"1/4", 1.0}, std::pair{"1/8", 0.5}, std::pair{"1/16", 0.25}, std::pair{"1/32", 0.125}})
            menu.addItem(name, [this, step] { roll_->quantize(step); });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&quantize));
    };
    clearClip.onClick = [this] { roll_->commitClip({}, "Clear clip " + juce::String(clipSlot() + 1)); };
    dragButton_ = &drag;
    clipInfo_ = &parts_.add<Caption>(*clips_, "", kTextSmall, true);
    clipItems_ = {{&trigger, 150, kChoiceH}, {&play(parts_.add<Toggle>(*clips_, ctx, "clip.latch", "Latch")), 80, 22, true},
                  {&play(parts_.add<Toggle>(*clips_, ctx, "clip.loop", "Loop")), 80, 22}, {&import, 128, 24, true}, {&exportButton, 128, 24},
                  {&drag, 128, 24, true}, {&quantize, 128, 24}, {&clearClip, 128, 24, true},
                  {&play(parts_.add<Toggle>(*clips_, ctx, "global.midi_out", "MIDI out")), 128, 22, true},
                  {&play(parts_.add<Toggle>(*clips_, ctx, "global.midi_thru", "MIDI thru")), 128, 22}, {clipInfo_, 260, 16, true}};
    roll_ = std::make_unique<PianoRoll>(ctx, *this);
    clips_->addAndMakeVisible(*roll_);
    auto ticker = std::make_unique<Ticker>([this] {
        const int slot = clipSlot();
        if (slot != seenSlot_) {
            seenSlot_ = slot;
            clipTabs_->repaint();
            const auto c = ctx_.processor.clip(slot);
            clipInfo_->setText(c.empty() ? juce::String("This slot is empty.") : str(c.name.empty() ? "Clip" : c.name) + ", " + juce::String(c.bars, 2) + " bars" + (c.key.empty() ? "" : ", " + str(c.key)));
            return true;
        }
        return false;
    });
    ticker->setBounds(0, 0, 1, 1);
    addAndMakeVisible(*ticker);
    ticker_ = std::move(ticker);
}

PlayPage::~PlayPage() = default;

int PlayPage::clipSlot() const { return juce::jlimit(0, 7, static_cast<int>(ctx_.processor.plainValue(idx("clip.slot"))) - 1); }
int PlayPage::patternSlot() const { return juce::jlimit(0, 11, static_cast<int>(ctx_.processor.plainValue(idx("arp.pattern"))) - 1); }

bool PlayPage::importClipFile(const juce::File& file, juce::String& error) {
    juce::MemoryBlock data;
    if (!file.loadFileAsData(data)) {
        error = "Could not read " + file.getFileName();
        return false;
    }
    const int slot = clipSlot();
    std::string problem;
    const bool ok = ctx_.session.perform("Import " + file.getFileName() + " into clip " + juce::String(slot + 1), [&] {
        problem = ctx_.processor.importClip(slot, std::span(static_cast<const std::uint8_t*>(data.getData()), data.getSize()));
        return problem.empty();
    });
    error = str(problem);
    seenSlot_ = -1;
    roll_->repaint();
    return ok;
}

bool PlayPage::exportClipFile(const juce::File& file) {
    const auto bytes = ctx_.processor.exportClip(clipSlot());
    return !bytes.empty() && file.replaceWithData(bytes.data(), bytes.size());
}

void PlayPage::paint(juce::Graphics& g) { g.fillAll(theme().bg); }

void PlayPage::resized() {
    auto area = getLocalBounds().reduced(8);
    auto top = area.removeFromTop(area.getHeight() / 2 - 20);
    chords_->setBounds(top.removeFromRight(440));
    top.removeFromRight(8);
    arp_->setBounds(top);
    area.removeFromTop(8);
    clips_->setBounds(area);
}

void PlayPage::mouseDrag(const juce::MouseEvent& e) {
    if (e.eventComponent != dragButton_ || e.getDistanceFromDragStart() < 6 || draggingOut_)
        return;
    draggingOut_ = true;
    const juce::File file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("Zederia clip " + juce::String(clipSlot() + 1) + ".mid");
    if (exportClipFile(file))
        juce::DragAndDropContainer::performExternalDragDropOfFiles({file.getFullPathName()}, false, this, [this] { draggingOut_ = false; });
    else
        draggingOut_ = false;
}

bool PlayPage::isInterestedInFileDrag(const juce::StringArray& files) {
    for (const auto& f : files)
        if (f.endsWithIgnoreCase(".mid") || f.endsWithIgnoreCase(".midi"))
            return true;
    return false;
}

void PlayPage::filesDropped(const juce::StringArray& files, int, int) {
    for (const auto& f : files)
        if (f.endsWithIgnoreCase(".mid") || f.endsWithIgnoreCase(".midi")) {
            juce::String error;
            if (!importClipFile(juce::File(f), error) && ctx_.notify)
                ctx_.notify(error);
            return;
        }
}

void PlayPage::structureChanged() {
    seenSlot_ = -1;
    if (lane_ != nullptr)
        lane_->repaint();
    if (roll_ != nullptr)
        roll_->repaint();
    if (chordEditor_ != nullptr)
        chordEditor_->repaint();
}

void PlayPage::applySimpleView() { resized(); }

bool PlayPage::reveal(const juce::String&) { return true; }

} // namespace zui

namespace zui {
juce::Component& PlayPage::pianoRoll() { return *roll_; }
juce::Component& PlayPage::stepLane() { return *lane_; }
} // namespace zui
