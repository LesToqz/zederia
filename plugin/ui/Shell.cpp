#include "Shell.h"

#include "Audit.h"
#include "../AgentLink.h"

namespace zui {

// --- TopBar ------------------------------------------------------------------------------------------

OutputPanel::OutputPanel(Context& ctx) : meter(ctx) {
    setTitle("Output");
    meter.vertical = true;
    master = std::make_unique<Knob>(ctx, "global.master", "Master");
    master->valueSize = kValueText;
    master->labelRow = 12.0f;
    master->trackInset = 3.5f; // a cap of about 23 px at 100 %
    addAndMakeVisible(meter);
    addAndMakeVisible(*master);
}

void OutputPanel::resized() {
    auto area = getLocalBounds();
    meter.setBounds(area.removeFromLeft(14).withSizeKeepingCentre(14, juce::jmin(area.getHeight() - 6, 56)));
    area.removeFromLeft(4);
    master->setBounds(area);
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
    presetName.textSize = kTextTitle;
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
    auto area = getLocalBounds().reduced(8, 6);
    menu.setBounds(area.removeFromLeft(28));
    area.removeFromLeft(6);
    prev.setBounds(area.removeFromLeft(22));
    presetName.setBounds(area.removeFromLeft(212));
    next.setBounds(area.removeFromLeft(22));
    area.removeFromLeft(4);
    save.setBounds(area.removeFromLeft(62));
    area.removeFromLeft(8);
    history.setBounds(area.removeFromLeft(26));
    area.removeFromLeft(14);
    abA.setBounds(area.removeFromLeft(28));
    area.removeFromLeft(2);
    abB.setBounds(area.removeFromLeft(28));
    area.removeFromLeft(4);
    abCopy.setBounds(area.removeFromLeft(44));
    // From the right (the output has a column of its own beyond the bar): agent, FX off, CPU, quality.
    agent.setBounds(area.removeFromRight(62));
    area.removeFromRight(8);
    fxOff->setBounds(area.removeFromRight(76));
    area.removeFromRight(8);
    cpuText.setBounds(area.removeFromRight(76));
    area.removeFromRight(6);
    quality->setBounds(area.removeFromRight(96));
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
