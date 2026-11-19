#include "Palette.h"

#include "Audit.h"

#include "../AgentLink.h"
#include "../PluginProcessor.h"

namespace zui {

// --- CommandPalette ----------------------------------------------------------------------------------

CommandPalette::CommandPalette(Context& ctx) : search("Find a control, action or preset", "Command palette search"), ctx_(ctx) {
    setTitle("Command palette");
    markOverlay(*this);
    list.setModel(this);
    list.setRowHeight(26);
    list.setTitle("Results");
    search.onTextChange = [this] { filter(); };
    search.onReturnKey = [this] { runMatch(juce::jmax(0, list.getSelectedRow())); };
    search.onEscapeKey = [this] { if (onClose) onClose(); };
    addAndMakeVisible(search);
    addAndMakeVisible(list);
}

void CommandPalette::setEntries(std::vector<Entry> entries) {
    entries_ = std::move(entries);
    filter();
}

void CommandPalette::open() {
    search.setText({}, false);
    filter();
    search.grabKeyboardFocus();
}

void CommandPalette::setQuery(const juce::String& text) {
    search.setText(text, false);
    filter();
}

void CommandPalette::filter() {
    matches_.clear();
    const auto words = juce::StringArray::fromTokens(search.getText().toLowerCase(), " ", "\"");
    std::vector<std::pair<int, int>> scored; // (score, index)
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        const auto& e = entries_[static_cast<std::size_t>(i)];
        const juce::String hay = (e.name + " " + e.detail + " " + e.kind).toLowerCase();
        bool all = true;
        int score = 0;
        for (const auto& w : words) {
            if (w.isEmpty())
                continue;
            const int at = hay.indexOf(w);
            if (at < 0) {
                all = false;
                break;
            }
            score += at == 0 ? 0 : e.name.toLowerCase().contains(w) ? 1 : 3;
        }
        if (all)
            scored.emplace_back(score * 4 + (e.kind == "Action" ? 0 : e.kind == "Page" ? 1 : e.kind == "Control" ? 2 : 3), i);
    }
    std::stable_sort(scored.begin(), scored.end());
    for (const auto& [score, i] : scored)
        matches_.push_back(i);
    list.updateContent();
    list.selectRow(0);
    list.repaint();
}

bool CommandPalette::runMatch(int i) {
    if (i < 0 || i >= matchCount())
        return false;
    const auto run = match(i).run;
    if (onClose)
        onClose();
    if (run)
        run();
    return true;
}

bool CommandPalette::keyPressed(const juce::KeyPress& key) {
    if (key == juce::KeyPress::escapeKey) {
        if (onClose)
            onClose();
        return true;
    }
    if (key == juce::KeyPress::downKey || key == juce::KeyPress::upKey) {
        list.selectRow(juce::jlimit(0, juce::jmax(0, matchCount() - 1), list.getSelectedRow() + (key == juce::KeyPress::downKey ? 1 : -1)));
        return true;
    }
    return false;
}

void CommandPalette::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) {
    if (row < 0 || row >= matchCount())
        return;
    const Theme& t = theme();
    const auto& e = match(row);
    if (selected) {
        g.setColour(t.selection);
        g.fillRect(0, 0, width, height);
    }
    auto area = juce::Rectangle<float>(10.0f, 0.0f, static_cast<float>(width) - 20.0f, static_cast<float>(height));
    drawText(g, e.kind, area.removeFromLeft(62.0f), kTextSmall, t.textDim);
    drawText(g, e.detail, area.removeFromRight(std::min(220.0f, area.getWidth() * 0.4f)),
             fittingSize(e.detail, std::min(220.0f, area.getWidth() * 0.4f), kTextSmall), t.textDim, juce::Justification::centredRight);
    drawText(g, e.name, area, fittingSize(e.name, area.getWidth(), kTextBody), t.text);
}

void CommandPalette::resized() {
    auto area = getLocalBounds().reduced(10);
    search.setBounds(area.removeFromTop(30));
    area.removeFromTop(6);
    list.setBounds(area);
}

void CommandPalette::paint(juce::Graphics& g) {
    const Theme& t = theme();
    g.setColour(t.raised);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), t.radius);
    g.setColour(t.focus);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), t.radius, 1.0f);
}

// --- PrefsPanel --------------------------------------------------------------------------------------

PrefsPanel::PrefsPanel(Context& ctx) : ctx_(ctx) {
    setTitle("Preferences");
    markOverlay(*this);
    close.setTitle("Close preferences");
    close.setTooltip("Close");
    close.ghost = true;
    close.setWantsKeyboardFocus(false); // Escape closes it; no focus outline when it opens
    close.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible(close);
    Prefs& p = ctx_.prefs;
    const auto save = [] { Prefs::get().save(); };
    addStepRow("Default size", "The size new editor windows open at, in percent of 1320 x 940. - and + step through the sizes.",
               [&p] { return juce::String(p.defaultScale) + " %"; },
               [this, &p, save](int direction) {
                   static const int sizes[] = {70, 85, 100, 125, 150, 175, 200, 250, 300};
                   int at = 0;
                   for (int i = 0; i < 9; ++i)
                       if (sizes[i] <= p.defaultScale)
                           at = i;
                   if (direction < 0 && sizes[at] == p.defaultScale)
                       --at;
                   else if (direction > 0)
                       ++at;
                   p.defaultScale = sizes[juce::jlimit(0, 8, at)];
                   save();
                   if (onScale) onScale(p.defaultScale);
               });
    addStepRow("Text size", "How large labels, values and lists are drawn, independent of the window size, from 90 to 150 % in steps of 5. "
                            "Where a larger text has no room it stays as large as fits, never below 100 %.",
               [&p] { return juce::String(p.textSize) + " %" + (p.textSize == 120 ? juce::String("  (default)") : juce::String()); },
               [this, &p, save](int direction) {
                   p.textSize = juce::jlimit(90, 150, (p.textSize / 5 + direction) * 5);
                   save();
                   if (onThemeChanged) onThemeChanged(); // fonts are part of the look
               });
    addRow("Knob drag", "Vertical: drag up and down. Around: follow the angle of the pointer around the knob.",
           [&p] { return p.rotaryDrag ? juce::String("Around") : juce::String("Vertical"); }, [&p, save](Button&) { p.rotaryDrag = !p.rotaryDrag; save(); });
    addRow("Mouse wheel", "What the wheel does over a control.", [&p] { return juce::String(p.wheel == 0 ? "Scrolls the page" : p.wheel == 1 ? "Changes values" : "Fine changes"); },
           [&p, save](Button&) { p.wheel = (p.wheel + 1) % 3; save(); });
    addRow("Double-click", "What a double-click on a control does.", [&p] { return p.doubleClickTypes ? juce::String("Type a value") : juce::String("Reset to default"); },
           [&p, save](Button&) { p.doubleClickTypes = !p.doubleClickTypes; save(); });
    addRow("Tooltips", "Small notes beside the pointer, besides the help strip.", [&p] { return p.tooltips ? juce::String("On") : juce::String("Off"); },
           [&p, save](Button&) { p.tooltips = !p.tooltips; save(); });
    addRow("Help strip", "The line at the bottom that explains what is under the pointer.", [&p] { return p.helpStrip ? juce::String("Shown") : juce::String("Hidden"); },
           [this, &p, save](Button&) { p.helpStrip = !p.helpStrip; save(); if (onLayout) onLayout(); });
    addRow("Computer keys", "The computer keyboard plays notes in two rows: Z S X D C V G B H N J M from C, Q 2 W 3 E R 5 T 6 Y 7 U an "
                            "octave higher. Left and Right move the octave, Down and Up the velocity.",
           [&p] { return p.computerKeys ? juce::String("Play notes") : juce::String("Off"); },
           [this, &p, save](Button&) { p.computerKeys = !p.computerKeys; save(); if (onLayout) onLayout(); });
    addRow("Key letters", "Shows on the keyboard which computer key plays each note.",
           [&p] { return p.keyLabels ? juce::String("Shown on the keys") : juce::String("Hidden"); },
           [this, &p, save](Button&) { p.keyLabels = !p.keyLabels; save(); if (onLayout) onLayout(); });
    addRow("View", "Simple view hides the advanced controls.", [&p] { return p.simpleView ? juce::String("Simple") : juce::String("Full"); },
           [this, &p, save](Button&) { p.simpleView = !p.simpleView; save(); if (onLayout) onLayout(); });
    addRow("Frame rate", "How often moving displays redraw. Lower costs less.", [&p] { return juce::String(p.frameCap) + " per second"; },
           [this, &p, save](Button&) { p.frameCap = p.frameCap == 15 ? 30 : p.frameCap == 30 ? 60 : 15; save(); if (onLayout) onLayout(); });
    addRow("Rendering", "Software rendering needs no graphics card; graphics-card rendering may be faster on some systems.",
           [&p] { return p.gpuRendering ? juce::String("Graphics card") : juce::String("Software"); },
           [this, &p, save](Button&) { p.gpuRendering = !p.gpuRendering; save(); if (onLayout) onLayout(); });
    addRow("MPE for new instances", "New instances start with MPE on.", [&p] { return p.mpeDefault ? juce::String("On") : juce::String("Off"); },
           [&p, save](Button&) { p.mpeDefault = !p.mpeDefault; save(); });
    addRow("Default preset", "What Init loads: the built-in init sound, or the current preset file.",
           [&p] { return p.defaultPreset.isEmpty() ? juce::String("Built-in init") : juce::File(p.defaultPreset).getFileNameWithoutExtension(); },
           [this, &p, save](Button&) {
               const juce::File current = ctx_.session.currentPreset();
               p.defaultPreset = p.defaultPreset.isEmpty() && current.existsAsFile() ? current.getFullPathName() : juce::String();
               save();
           });
    addRow("Content folder", "Where your presets, tables and other files are kept.", [&p] { return p.userContentFolder().getFullPathName(); },
           [this, &p, save](Button&) {
               auto chooser = std::make_shared<juce::FileChooser>("Content folder", p.userContentFolder());
               chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this, chooser, &p, save](const juce::FileChooser& fc) {
                   if (fc.getResult() != juce::File()) {
                       p.userFolder = fc.getResult().getFullPathName();
                       save();
                       refresh();
                   }
               });
           });
    addRow("Agent link", "Lets AI agents work on this instance. Switch it, and connect an agent, on the AGENT page.",
           [this] { return ctx_.processor.agentLink().enabled() ? juce::String("On (AGENT page)") : juce::String("Off (AGENT page)"); }, [](Button&) {});
    offlineQuality = std::make_unique<ChoiceBox>(ctx, "global.quality_offline", "Offline quality");
    offlineQuality->labelAbove = false;
    addAndMakeVisible(*offlineQuality);
    author.setText(p.author, false);
    author.onTextChange = [this] {
        ctx_.prefs.author = author.getText();
        Prefs::get().save();
    };
    addAndMakeVisible(author);
}

void PrefsPanel::addRow(const juce::String& label, const juce::String& help, std::function<juce::String()> current, std::function<void(Button&)> change) {
    auto row = std::make_unique<Row>();
    row->label = label;
    row->help = help;
    row->current = std::move(current);
    row->change = std::move(change);
    row->value = std::make_unique<Button>(row->current());
    row->value->setTitle(label);
    row->value->setHelp(label, help);
    Row* r = row.get();
    row->value->onClick = [this, r] {
        r->change(*r->value);
        refresh();
    };
    addAndMakeVisible(*row->value);
    rows_.push_back(std::move(row));
}

void PrefsPanel::addStepRow(const juce::String& label, const juce::String& help, std::function<juce::String()> current, std::function<void(int)> step) {
    addRow(label, help, std::move(current), [](Button&) {}); // the value itself only shows
    Row* r = rows_.back().get();
    r->value->setInterceptsMouseClicks(false, false);
    r->value->setWantsKeyboardFocus(false);
    r->step = std::move(step);
    for (const int direction : {-1, 1}) {
        auto& b = direction < 0 ? r->minus : r->plus;
        b = std::make_unique<Button>("", direction < 0 ? Glyph::Minus : Glyph::Plus);
        b->setTitle((direction < 0 ? "Smaller " : "Larger ") + label.toLowerCase());
        b->setTooltip(b->getTitle());
        b->setHelp(label, help);
        b->onClick = [this, r, direction] {
            r->step(direction);
            refresh();
        };
        addAndMakeVisible(*b);
    }
}

void PrefsPanel::refresh() {
    for (auto& r : rows_)
        r->value->setText(r->current());
    repaint();
}

void PrefsPanel::resized() {
    auto area = getLocalBounds().reduced(16, 12);
    close.setBounds(area.removeFromTop(24).removeFromRight(24));
    area.removeFromTop(6);
    const int labelW = 170;
    for (auto& r : rows_) {
        auto line = area.removeFromTop(28);
        line.removeFromLeft(labelW);
        auto box = line.removeFromLeft(juce::jmin(line.getWidth(), 300)).reduced(0, 2);
        if (r->minus != nullptr) { // - value +
            r->minus->setBounds(box.removeFromLeft(32));
            r->plus->setBounds(box.removeFromRight(32));
            box.reduce(4, 0);
        }
        r->value->setBounds(box);
        area.removeFromTop(2);
    }
    auto line = area.removeFromTop(28);
    line.removeFromLeft(labelW);
    author.setBounds(line.removeFromLeft(300).reduced(0, 2));
    area.removeFromTop(4);
    line = area.removeFromTop(28);
    offlineQuality->setBounds(line.withTrimmedLeft(0).removeFromLeft(labelW + 300).reduced(0, 2));
}

void PrefsPanel::paint(juce::Graphics& g) {
    const Theme& t = theme();
    g.setColour(t.raised);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), t.radius);
    g.setColour(t.lineStrong);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), t.radius, 1.0f);
    auto area = getLocalBounds().reduced(16, 12);
    drawText(g, "PREFERENCES", area.removeFromTop(24).toFloat(), kTextTitle, t.text, juce::Justification::centredLeft, true);
    area.removeFromTop(6);
    for (const auto& r : rows_) {
        drawText(g, r->label, area.removeFromTop(28).toFloat().withWidth(165.0f), kTextBody, t.textDim);
        area.removeFromTop(2);
    }
    drawText(g, "Default author", area.removeFromTop(28).toFloat().withWidth(165.0f), kTextBody, t.textDim);
}

} // namespace zui
