#include "Shell.h"

#include "Audit.h"
#include "../AgentLink.h"

namespace zui {

// --- TopBar ------------------------------------------------------------------------------------------

OutputPanel::OutputPanel(Context& ctx) : scope(ctx), meter(ctx) {
    setTitle("Output");
    meter.vertical = true;
    master = std::make_unique<Knob>(ctx, "global.master", "Master");
    master->valueSize = kPrimaryValueText;
    master->labelRow = 13.0f;
    master->trackInset = 2.0f;
    addAndMakeVisible(scope);
    addAndMakeVisible(meter);
    addAndMakeVisible(*master);
}

void OutputPanel::resized() {
    auto area = getLocalBounds().reduced(4, 0);
    master->setBounds(area.removeFromRight(62));
    area.removeFromRight(3);
    meter.setBounds(area.removeFromRight(15).reduced(0, 5));
    area.removeFromRight(7);
    scope.setBounds(area.reduced(0, 3));
}

Nameplate::Nameplate(Context& ctx) : ctx_(ctx), polyParam_(zed::Registry::get().indexOf("global.polyphony")),
                                      modeParam_(zed::Registry::get().indexOf("global.mode")) {
    setTitle("Zederia voices");
    setHelp("Voice allocation", "Sounding voices, including releases, and the current voice limit.");
    tick();
}

bool Nameplate::tick() {
    const int active = ctx_.processor.activeVoices();
    const int limit = ctx_.processor.plainValue(modeParam_) < 0.5 ? juce::roundToInt(ctx_.processor.plainValue(polyParam_)) : 1;
    if (active == active_ && limit == polyphony_)
        return false;
    active_ = active;
    polyphony_ = limit;
    return true;
}

void Nameplate::draw(juce::Graphics& g) {
    const auto bounds = getLocalBounds();
    const auto& t = theme();
    plate_.draw(g, bounds, 0, true, [&](juce::Graphics& pg) {
        drawPanel(pg, bounds.toFloat(), t.bg);
        // Seven engraved letterforms share the same angled cuts as the shoulder of the Z.
        juce::Path z, letters;
        z.startNewSubPath(0, 0); z.lineTo(21, 0); z.lineTo(21, 3); z.lineTo(7, 15); z.lineTo(21, 15);
        z.lineTo(18, 18); z.lineTo(0, 18); z.lineTo(0, 15); z.lineTo(14, 3); z.lineTo(0, 3); z.closeSubPath();
        const auto e = [&](float x) {
            letters.startNewSubPath(x + 11, 0); letters.lineTo(x, 0); letters.lineTo(x, 16); letters.lineTo(x + 11, 16);
            letters.startNewSubPath(x, 8); letters.lineTo(x + 8, 8);
        };
        e(28);
        letters.startNewSubPath(45, 0); letters.lineTo(53, 0); letters.lineTo(58, 4); letters.lineTo(58, 12);
        letters.lineTo(53, 16); letters.lineTo(45, 16); letters.closeSubPath();
        e(64);
        letters.startNewSubPath(81, 16); letters.lineTo(81, 0); letters.lineTo(91, 0); letters.lineTo(94, 3);
        letters.lineTo(94, 6); letters.lineTo(91, 9); letters.lineTo(82, 9);
        letters.startNewSubPath(88, 9); letters.lineTo(95, 16);
        letters.startNewSubPath(101, 0); letters.lineTo(101, 16);
        letters.startNewSubPath(107, 16); letters.lineTo(113, 0); letters.lineTo(116, 0); letters.lineTo(122, 16);
        letters.startNewSubPath(110, 10); letters.lineTo(119, 10);
        const auto place = juce::AffineTransform::translation(bounds.getCentreX() - 61.0f, 15.0f);
        pg.setColour(t.lamp);
        pg.fillPath(z, place);
        pg.setColour(t.text);
        pg.strokePath(letters, juce::PathStrokeType(2.6f, juce::PathStrokeType::mitered, juce::PathStrokeType::square), place.translated(0.0f, 1.0f));
    });
    auto readout = bounds.toFloat().reduced(14.0f, 0.0f).removeFromBottom(27.0f).withHeight(17.0f);
    drawText(g, "VOICES", readout.removeFromLeft(42.0f), kTextSmall, t.textDim, juce::Justification::centredLeft, true);
    const auto count = readout.removeFromRight(40.0f);
    drawText(g, juce::String(active_) + " / " + juce::String(polyphony_), count, kTextSmall, t.text, juce::Justification::centredRight);
    const int lit = (active_ * 8 + polyphony_ - 1) / std::max(1, polyphony_);
    for (int i = 0; i < 8; ++i) {
        g.setColour(i < lit ? t.lamp : t.line);
        g.fillRect(readout.getX() + i * 4.0f, readout.getCentreY() - 2.0f, 2.5f, 4.0f);
    }
}

TopBar::TopBar(Context& ctx) : ctx_(ctx) {
    setTitle("Top bar");
    menu.setTitle("Menu");
    menu.setHelp("Menu", "Init, open and save presets, MIDI learn and controller maps, programs, preferences, theme and size.");
    menu.setTooltip("Menu");
    prev.setTitle("Previous preset");
    prev.setTooltip("Previous preset");
    next.setTitle("Next preset");
    next.setTooltip("Next preset");
    presetName.setHelp("Preset", "The sound's name; click to open the library. A dot marks changes since it was loaded or saved.");
    save.setHelp("Save", "Save the sound as a preset (Ctrl+S).");
    history.setTitle("History");
    history.setTooltip("History: every change, by name");
    history.setHelp("History", "The list of changes; click one to go back to it. Ctrl+Z undoes, Ctrl+Shift+Z redoes.");
    abA.setHelp("Compare A", "Two versions of the sound to compare. Click the other letter to switch (Ctrl+B).");
    abB.setHelp("Compare B", "Two versions of the sound to compare. Click the other letter to switch (Ctrl+B).");
    abCopy.setHelp("Copy to other side", "Copy this side to the other side of the compare.");
    abA.toggleRole = abB.toggleRole = true;
    quality = std::make_unique<ChoiceBox>(ctx, "global.quality", "Quality");
    quality->labelAbove = false;
    quality->showLabel = false;
    quality->setHelp("Quality", "Oversampling while playing: Draft costs least, Ultra the most. Offline renders use the offline quality (preferences).");
    fxOff = std::make_unique<Toggle>(ctx, "global.fx_off", "FX off");
    cpuText.setTitle("CPU");
    agent.setTitle("Agent link");
    for (juce::Component* c : std::initializer_list<juce::Component*>{&menu, &prev, &next, &presetName, &save, &history, &abA, &abB,
                                                                       &abCopy, quality.get(), &cpuText, fxOff.get(), &agent})
        addAndMakeVisible(c);
    for (Button* b : {&menu, &prev, &next, &history})
        b->ghost = true;
    presetName.ghost = true;
    presetName.textSize = kTextLarge;
    presetName.boldText = true;
    abA.onClick = [this] {
        if (ctx_.session.abSide() != 0)
            ctx_.session.switchAB();
    };
    abB.onClick = [this] {
        if (ctx_.session.abSide() != 1)
            ctx_.session.switchAB();
    };
    abCopy.onClick = [this] {
        ctx_.session.copyToOtherSide();
        if (ctx_.notify)
            ctx_.notify(ctx_.session.abSide() == 0 ? "A copied to B" : "B copied to A");
    };
    history.onClick = [this] { if (onHistory) onHistory(); };
    menu.onClick = [this] { if (onMenu) onMenu(); };
    save.onClick = [this] { if (onSavePreset) onSavePreset(); };
    presetName.onClick = [this] { if (onPresetBrowse) onPresetBrowse(); };
    prev.onClick = [this] { if (onStepPreset) onStepPreset(-1); };
    next.onClick = [this] { if (onStepPreset) onStepPreset(1); };
    refresh();
}

void TopBar::refresh() {
    const juce::String name = ctx_.session.presetName() + (ctx_.session.modified() ? juce::String::fromUTF8(" \xe2\x80\xa2") : juce::String());
    presetName.setText(name);
    presetName.setTitle("Preset " + ctx_.session.presetName());
    abA.setToggled(ctx_.session.abSide() == 0);
    abB.setToggled(ctx_.session.abSide() == 1);
    abCopy.setText(ctx_.session.abSide() == 0 ? "A>B" : "B>A");
    abCopy.setTitle(ctx_.session.abSide() == 0 ? "Copy A to B" : "Copy B to A");
    agent.setText(ctx_.processor.agentLink().enabled() ? "Agent on" : "Agent off");
    repaint();
}

// The CPU readout changes at most twice a second, and only when the number it shows changes: a
// figure that changes every frame cannot be read.
bool TopBar::tick() {
    // The indicator follows the instance's Agent Link (plan 4.8: the top bar shows when it is on).
    const juce::String agentText = ctx_.processor.agentLink().enabled() ? "Agent on" : "Agent off";
    if (agentText != agent.text()) {
        agent.setText(agentText);
        agent.setTooltip(agentText == "Agent on" ? "Agent Link is on: AI agents on this computer can edit this instance (AGENT page)"
                                                 : "Agent Link is off (AGENT page)");
        return true;
    }
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - cpuShownAt_ < 500.0)
        return false;
    const float cpu = ctx_.processor.cpuAverage();
    const juce::String text = "CPU " + juce::String(cpu, 1) + " %";
    if (text == cpuText.text())
        return false;
    cpuShownAt_ = now;
    const float peak = ctx_.processor.cpuPeak();
    cpuText.setText(text);
    cpuText.setTooltip("Audio callback time against real time: " + juce::String(cpu, 1) + " % average, " + juce::String(peak, 1) + " % peak");
    return true;
}

void TopBar::resized() {
    auto area = getLocalBounds().reduced(6, 5);
    menu.setBounds(area.removeFromLeft(28));
    area.removeFromLeft(5);
    agent.setBounds(area.removeFromRight(57));
    area.removeFromRight(6);
    fxOff->setBounds(area.removeFromRight(68));
    area.removeFromRight(4);
    cpuText.setBounds(area.removeFromRight(76));
    area.removeFromRight(4);
    quality->setBounds(area.removeFromRight(82));
    area.removeFromRight(12);
    abCopy.setBounds(area.removeFromRight(40));
    area.removeFromRight(2);
    abB.setBounds(area.removeFromRight(27));
    abA.setBounds(area.removeFromRight(27));
    area.removeFromRight(8);
    history.setBounds(area.removeFromRight(26));
    save.setBounds(area.removeFromRight(60));
    area.removeFromRight(5);
    prev.setBounds(area.removeFromLeft(25));
    next.setBounds(area.removeFromRight(25));
    presetName.setBounds(area);
}

void TopBar::paint(juce::Graphics&) {} // the editor's cached header draws the plate and the preset window behind

juce::Rectangle<int> TopBar::presetWindow() const {
    return juce::Rectangle<int>(prev.getX() - 2, presetName.getY() - 1, next.getRight() - prev.getX() + 4, presetName.getHeight() + 2);
}

// --- MacroStrip --------------------------------------------------------------------------------------

MacroStrip::MacroStrip(Context& ctx) : ctx_(ctx) {
    setTitle("Macros");
    for (int m = 0; m < zed::kNumMacros; ++m) {
        auto knob = std::make_unique<Knob>(ctx, zed::Registry::get().macroValue(m), "");
        knob->small = true;
        knob->showValue = false;
        knob->showLabel = false;
        knob->labelledBy = "Macro " + juce::String(m + 1);
        knob->family = Family::Modulators;
        auto chip = std::make_unique<DragChip>(ctx, "macro" + juce::String(m + 1));
        auto name = std::make_unique<Button>("Macro " + juce::String(m + 1));
        name->ghost = true;
        name->textSize = kTextSmall;
        name->setHelp("Macro name", "Click to rename the macro.");
        name->onClick = [this, m] {
            auto editor = std::make_shared<juce::AlertWindow>("Rename macro", "A name for macro " + juce::String(m + 1), juce::MessageBoxIconType::NoIcon);
            const auto names = ctx_.processor.macroNames();
            editor->addTextEditor("name", str(names[static_cast<std::size_t>(m)]).isEmpty() ? "Macro " + juce::String(m + 1) : str(names[static_cast<std::size_t>(m)]));
            editor->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
            editor->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            editor->enterModalState(true, juce::ModalCallbackFunction::create([this, editor, m](int result) {
                if (result != 1)
                    return;
                const std::string text = editor->getTextEditorContents("name").substring(0, 32).toStdString();
                ctx_.session.edit("Rename macro " + juce::String(m + 1), [&](zed::Patch& p) {
                    p.macroNames[static_cast<std::size_t>(m)] = text;
                    return true;
                });
                refreshNames();
            }), false);
        };
        auto bipolar = std::make_unique<Segmented>(ctx, zed::Registry::get().macroBipolar(m), "Bipolar");
        bipolar->names = {"+", "+/-"};
        bipolar->showLabel = false;
        bipolar->labelledBy = "Macro " + juce::String(m + 1);
        bipolar->family = Family::Modulators;
        bipolar->setHelp("Macro range", "0 to 1, or -1 to 1 around the middle.");
        addAndMakeVisible(*bipolar);
        polarity.push_back(std::move(bipolar));
        addAndMakeVisible(*knob);
        addAndMakeVisible(*chip);
        addAndMakeVisible(*name);
        knobs.push_back(std::move(knob));
        chips.push_back(std::move(chip));
        names.push_back(std::move(name));
    }
    refreshNames();
}

void MacroStrip::refreshNames() {
    const auto list = ctx_.processor.macroNames();
    for (int m = 0; m < zed::kNumMacros; ++m) {
        const juce::String n = str(list[static_cast<std::size_t>(m)]);
        const juce::String shown = n.isEmpty() ? "Macro " + juce::String(m + 1) : n;
        names[static_cast<std::size_t>(m)]->setText(shown);
        names[static_cast<std::size_t>(m)]->setTitle("Rename " + shown);
        knobs[static_cast<std::size_t>(m)]->setTitle(shown);
        chips[static_cast<std::size_t>(m)]->text = shown;
        chips[static_cast<std::size_t>(m)]->setTitle(shown + " modulation source");
    }
    repaint();
}

void MacroStrip::resized() {
    auto area = getLocalBounds().reduced(8, 4);
    const int each = area.getWidth() / zed::kNumMacros;
    for (int m = 0; m < zed::kNumMacros; ++m) {
        auto cell = area.removeFromLeft(each).reduced(4, 0);
        knobs[static_cast<std::size_t>(m)]->setBounds(cell.removeFromLeft(36).withSizeKeepingCentre(36, 42));
        cell.removeFromLeft(4);
        auto top = cell.removeFromTop(20);
        polarity[static_cast<std::size_t>(m)]->setBounds(top.removeFromRight(40));
        top.removeFromRight(2);
        names[static_cast<std::size_t>(m)]->setBounds(top);
        cell.removeFromTop(2);
        chips[static_cast<std::size_t>(m)]->setBounds(cell.removeFromTop(18));
    }
}

void MacroStrip::paint(juce::Graphics& g) {
    const Theme& t = theme();
    g.setColour(t.panel);
    g.fillRect(getLocalBounds());
    g.setColour(t.line);
    g.fillRect(getLocalBounds().removeFromTop(1));
}

// --- HelpStrip ---------------------------------------------------------------------------------------

HelpStrip::HelpStrip() {
    setTitle("Help");
    setInterceptsMouseClicks(false, false);
}

void HelpStrip::show(const juce::String& title, const juce::String& text, const juce::String& value) {
    if (pinned || (title == title_ && text == text_ && value == value_))
        return;
    title_ = title;
    text_ = text;
    value_ = value;
    repaint();
}

void HelpStrip::message(const juce::String& text) {
    message_ = text;
    messageUntil_ = juce::Time::getMillisecondCounterHiRes() + 4000.0;
    if (messageOnly) { // the strip is hidden: it shows over the bottom edge while the message lasts
        setVisible(true);
        toFront(false);
    }
    repaint();
}

bool HelpStrip::tick() {
    if (message_.isNotEmpty() && juce::Time::getMillisecondCounterHiRes() > messageUntil_) {
        message_.clear();
        if (messageOnly)
            setVisible(false);
        repaint();
        return true;
    }
    return false;
}

void HelpStrip::paint(juce::Graphics& g) {
    const Theme& t = theme();
    g.setColour(t.sunken);
    g.fillRect(getLocalBounds());
    auto area = getLocalBounds().toFloat().reduced(10.0f, 0.0f);
    if (message_.isNotEmpty()) {
        drawText(g, message_, area, kTextSmall + 0.5f, t.focus);
        return;
    }
    const juce::String hint = pinned ? "Pinned (F1)" : "F1 pins this line";
    drawText(g, hint, area.removeFromRight(110.0f), kTextSmall, t.textDim, juce::Justification::centredRight);
    if (value_.isNotEmpty())
        drawText(g, value_, area.removeFromRight(120.0f), kTextSmall + 0.5f, t.text, juce::Justification::centredRight);
    if (title_.isNotEmpty()) {
        const float w = std::min(textWidth(title_, kTextSmall + 0.5f, true) + 12.0f, area.getWidth() * 0.35f);
        drawText(g, title_, area.removeFromLeft(w), kTextSmall + 0.5f, t.focus, juce::Justification::centredLeft, true);
    }
    const juce::String shown = text_.isNotEmpty() ? text_ : juce::String("Point at anything to read what it does. Ctrl+K finds any control, action or preset.");
    drawText(g, shown, area, fittingSize(shown, area.getWidth(), kTextSmall + 0.5f), t.textDim);
}

// --- HistoryPanel ------------------------------------------------------------------------------------

HistoryPanel::HistoryPanel(Context& ctx) : ctx_(ctx) {
    setTitle("History");
    markOverlay(*this);
    list.setModel(this);
    list.setRowHeight(24);
    list.setTitle("Undo history");
    addAndMakeVisible(list);
    close.setTitle("Close history");
    close.setTooltip("Close");
    close.ghost = true;
    close.setWantsKeyboardFocus(false); // Escape closes it; no focus outline when it opens
    close.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible(close);
}

void HistoryPanel::refresh() {
    list.updateContent();
    list.selectRow(ctx_.session.history().current());
    list.scrollToEnsureRowIsOnscreen(ctx_.session.history().current());
    repaint();
}

int HistoryPanel::getNumRows() { return ctx_.session.history().size(); }

void HistoryPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool) {
    const Theme& t = theme();
    const auto& h = ctx_.session.history();
    if (row < 0 || row >= h.size())
        return;
    const bool current = row == h.current();
    const bool future = row > h.current();
    if (current) {
        g.setColour(t.selection);
        g.fillRect(0, 0, width, height);
    }
    const juce::String origin = str(h.origin(row));
    const juce::String text = juce::String(row) + "  " + str(h.label(row)) + (origin.isNotEmpty() ? "  (" + origin + ")" : juce::String());
    drawText(g, text, juce::Rectangle<float>(8.0f, 0.0f, static_cast<float>(width) - 16.0f, static_cast<float>(height)),
             fittingSize(text, static_cast<float>(width) - 16.0f, kTextSmall + 0.5f), future ? t.textFaint : t.text);
}

void HistoryPanel::listBoxItemClicked(int row, const juce::MouseEvent&) {
    ctx_.session.jumpTo(row);
    refresh();
}

void HistoryPanel::returnKeyPressed(int row) {
    ctx_.session.jumpTo(row);
    refresh();
}

void HistoryPanel::resized() {
    auto area = getLocalBounds().reduced(8);
    auto top = area.removeFromTop(24);
    close.setBounds(top.removeFromRight(24));
    area.removeFromTop(4);
    list.setBounds(area);
}

void HistoryPanel::paint(juce::Graphics& g) {
    const Theme& t = theme();
    drawPanel(g, getLocalBounds().toFloat(), t.raised);
    g.setColour(t.lineStrong);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), t.radius, 1.0f);
    const auto& h = ctx_.session.history();
    drawText(g, "HISTORY  " + juce::String(h.size() - 1) + " changes" + (h.dropped() > 0 ? ", oldest dropped" : ""),
             getLocalBounds().reduced(10, 8).removeFromTop(24).toFloat(), kTextSmall + 0.5f, t.text, juce::Justification::centredLeft, true);
}

} // namespace zui
