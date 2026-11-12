#include "PluginEditor.h"

#include "zederia/Version.h"

namespace {
const juce::Colour kBackground(0xff17191c);
const juce::Colour kPanel(0xff202327);
const juce::Colour kText(0xffd8dce2);
const juce::Colour kDim(0xff8a9099);
const juce::Colour kAccent(0xff58c4a8);
} // namespace

// One parameter: id, name, a slider over the normalized range, and the value as text.
class ZederiaEditor::Row final : public juce::Component {
public:
    explicit Row(ZederiaProcessor& processor) : processor_(processor) {
        for (auto* label : {&id_, &name_, &value_}) {
            label->setFont(juce::FontOptions(13.0f));
            label->setMinimumHorizontalScale(0.7f);
            addAndMakeVisible(*label);
        }
        id_.setColour(juce::Label::textColourId, kAccent);
        name_.setColour(juce::Label::textColourId, kText);
        value_.setColour(juce::Label::textColourId, kText);
        value_.setJustificationType(juce::Justification::centredRight);

        slider_.setSliderStyle(juce::Slider::LinearHorizontal);
        slider_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        slider_.setRange(0.0, 1.0);
        slider_.setColour(juce::Slider::trackColourId, kAccent);
        slider_.onDragStart = [this] { processor_.beginGesture(index_); };
        slider_.onDragEnd = [this] { processor_.endGesture(index_); };
        slider_.onValueChange = [this] {
            if (!updating_)
                processor_.setNormalizedFromUi(index_, static_cast<float>(slider_.getValue()));
            showValue();
        };
        addAndMakeVisible(slider_);
    }

    void setIndex(int index) {
        index_ = index;
        const auto& def = zed::Registry::get().at(index);
        id_.setText(def.id, juce::dontSendNotification);
        slider_.setTitle(def.name);
        slider_.setHelpText(def.description);
        setTooltipText(def.description);
        refresh();
    }

    // Follows changes made elsewhere (host automation, a loaded patch).
    void refresh() {
        if (index_ < 0)
            return;
        const auto& own = zed::Registry::get().at(index_);
        const auto& def = processor_.effectiveDef(index_);
        name_.setText(&def == &own ? own.name : own.name + " (" + def.name + ")", juce::dontSendNotification);
        slider_.setDoubleClickReturnValue(true, def.toNormalized(def.def));
        if (!slider_.isMouseButtonDown()) {
            const juce::ScopedValueSetter<bool> guard(updating_, true);
            slider_.setValue(processor_.normalizedValue(index_), juce::sendNotificationSync);
        }
        showValue();
    }

    void resized() override {
        auto area = getLocalBounds().reduced(6, 2);
        id_.setBounds(area.removeFromLeft(juce::jmin(210, area.getWidth() / 4)));
        value_.setBounds(area.removeFromRight(juce::jmin(130, area.getWidth() / 4)));
        name_.setBounds(area.removeFromLeft(juce::jmin(230, area.getWidth() / 2)));
        slider_.setBounds(area);
    }

private:
    void showValue() {
        if (index_ < 0)
            return;
        const auto& def = processor_.effectiveDef(index_);
        value_.setText(def.format(processor_.plainValue(index_)), juce::dontSendNotification);
    }
    void setTooltipText(const std::string& text) {
        id_.setTooltip(text);
        name_.setTooltip(text);
    }

    ZederiaProcessor& processor_;
    int index_ = -1;
    bool updating_ = false;
    juce::Label id_, name_, value_;
    juce::Slider slider_;
};

// Peak level of both channels on a decibel scale, with a slow fall.
class ZederiaEditor::Meter final : public juce::Component {
public:
    void push(float left, float right) {
        const float fall = 0.85f;
        level_[0] = juce::jmax(left, level_[0] * fall);
        level_[1] = juce::jmax(right, level_[1] * fall);
        repaint();
    }
    float level(int channel) const { return level_[channel]; }

    void paint(juce::Graphics& g) override {
        const auto area = getLocalBounds().toFloat();
        const float barHeight = (area.getHeight() - 2.0f) / 2.0f;
        for (int channel = 0; channel < 2; ++channel) {
            const auto bar = juce::Rectangle<float>(area.getX(), area.getY() + static_cast<float>(channel) * (barHeight + 2.0f),
                                                    area.getWidth(), barHeight);
            g.setColour(kPanel);
            g.fillRect(bar);
            const float db = juce::Decibels::gainToDecibels(level_[channel], -60.0f);
            const float fraction = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 66.0f); // -60 .. +6 dB
            g.setColour(db > 0.0f ? juce::Colours::orangered : kAccent);
            g.fillRect(bar.withWidth(bar.getWidth() * fraction));
        }
        g.setColour(kDim);
        const float zero = area.getX() + area.getWidth() * 60.0f / 66.0f; // the 0 dB mark
        g.drawVerticalLine(juce::roundToInt(zero), area.getY(), area.getBottom());
    }

private:
    float level_[2] = {0.0f, 0.0f};
};

ZederiaEditor::ZederiaEditor(ZederiaProcessor& processor)
    : juce::AudioProcessorEditor(processor), processor_(processor), list_("Parameters", this),
      meter_(std::make_unique<Meter>()),
      keyboard_(processor.keyboardState(), juce::MidiKeyboardComponent::horizontalKeyboard) {
    title_.setText(juce::String("Zederia ") + zed::kVersion + "  -  temporary editor", juce::dontSendNotification);
    title_.setFont(juce::FontOptions(17.0f, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, kText);
    addAndMakeVisible(title_);

    status_.setFont(juce::FontOptions(13.0f));
    status_.setColour(juce::Label::textColourId, kDim);
    status_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(status_);

    search_.setTextToShowWhenEmpty("Search parameters by id or name, for example  osc1  or  cutoff", kDim);
    search_.setTitle("Search parameters");
    search_.onTextChange = [this] { rebuildRows(); };
    addAndMakeVisible(search_);

    static const char* const assetNames[4] = {"Osc 1 table", "Osc 2 table", "Osc 3 table", "Texture"};
    for (int slot = 0; slot < 4; ++slot) {
        assetLabel_[slot].setText(assetNames[slot], juce::dontSendNotification);
        assetLabel_[slot].setFont(juce::FontOptions(13.0f));
        assetLabel_[slot].setColour(juce::Label::textColourId, kDim);
        addAndMakeVisible(assetLabel_[slot]);
        assetBox_[slot].setTitle(assetNames[slot]);
        assetBox_[slot].onChange = [this, slot] {
            if (fillingBoxes_)
                return;
            const int item = assetBox_[slot].getSelectedItemIndex();
            if (item < 0 || item >= static_cast<int>(assetRefs_[slot].size()))
                return;
            const std::string& reference = assetRefs_[slot][static_cast<std::size_t>(item)];
            const std::string problem = slot < 3 ? processor_.setTableReference(slot, reference) : processor_.setTextureReference(reference);
            if (!problem.empty())
                status_.setText(problem, juce::dontSendNotification);
        };
        addAndMakeVisible(assetBox_[slot]);
    }
    refreshAssetBoxes();

    list_.setRowHeight(26);
    list_.setColour(juce::ListBox::backgroundColourId, kPanel);
    list_.setTitle("Parameters");
    addAndMakeVisible(list_);

    meter_->setTitle("Output level");
    addAndMakeVisible(*meter_);

    keyboard_.setAvailableRange(24, 108);
    keyboard_.setOctaveForMiddleC(4);
    keyboard_.setTitle("Keyboard");
    addAndMakeVisible(keyboard_);

    rebuildRows();
    setResizable(true, true);
    setResizeLimits(640, 420, 1800, 1400);
    setSize(980, 680);
    startTimerHz(30);
}

ZederiaEditor::~ZederiaEditor() {
    stopTimer();
    // Keys still held with the mouse must not hang when the window closes.
    processor_.keyboardState().allNotesOff(0);
}

void ZederiaEditor::paint(juce::Graphics& g) { g.fillAll(kBackground); }

void ZederiaEditor::resized() {
    auto area = getLocalBounds().reduced(10);
    auto top = area.removeFromTop(28);
    title_.setBounds(top.removeFromLeft(330));
    meter_->setBounds(top.removeFromRight(220).reduced(0, 5));
    status_.setBounds(top);
    area.removeFromTop(6);
    keyboard_.setBounds(area.removeFromBottom(90));
    keyboard_.setKeyWidth(juce::jmax(8.0f, static_cast<float>(keyboard_.getWidth()) / 50.0f)); // 50 white keys, C1 to C8
    area.removeFromBottom(8);
    auto assets = area.removeFromTop(24);
    const int each = assets.getWidth() / 4;
    for (int slot = 0; slot < 4; ++slot) {
        auto cell = assets.removeFromLeft(each).reduced(2, 0);
        assetLabel_[slot].setBounds(cell.removeFromLeft(juce::jmin(84, cell.getWidth() / 2)));
        assetBox_[slot].setBounds(cell);
    }
    area.removeFromTop(6);
    search_.setBounds(area.removeFromTop(26));
    area.removeFromTop(6);
    list_.setBounds(area);
}

void ZederiaEditor::setSearchText(const juce::String& text) {
    search_.setText(text, juce::dontSendNotification);
    rebuildRows();
}

// Lists what the content folders hold and selects what the patch names.
void ZederiaEditor::refreshAssetBoxes() {
    const juce::ScopedValueSetter<bool> guard(fillingBoxes_, true);
    const zed::ContentLibrary library = processor_.library();
    const std::vector<std::string> tables = library.listTables(), textures = library.listTextures();
    for (int slot = 0; slot < 4; ++slot) {
        const std::string current = slot < 3 ? processor_.tableReference(slot) : processor_.textureReference();
        auto& refs = assetRefs_[slot];
        refs.assign(1, std::string());
        refs.insert(refs.end(), (slot < 3 ? tables : textures).begin(), (slot < 3 ? tables : textures).end());
        if (!current.empty() && std::find(refs.begin(), refs.end(), current) == refs.end())
            refs.push_back(current); // named by the patch but not in the folders: still shown
        assetBox_[slot].clear(juce::dontSendNotification);
        int selected = 0;
        for (std::size_t i = 0; i < refs.size(); ++i) {
            assetBox_[slot].addItem(i == 0 ? juce::String(slot < 3 ? "(built-in: Basic Shapes)" : "(built-in: White Noise)") : juce::String(refs[i]),
                                    static_cast<int>(i) + 1);
            if (refs[i] == current)
                selected = static_cast<int>(i);
        }
        assetBox_[slot].setSelectedItemIndex(selected, juce::dontSendNotification);
    }
}

void ZederiaEditor::rebuildRows() {
    const auto& reg = zed::Registry::get();
    const auto needle = search_.getText().trim().toLowerCase();
    rows_.clear();
    for (const auto& def : reg.params()) {
        const auto& effective = processor_.effectiveDef(def.index);
        if (needle.isEmpty() || juce::String(def.id).contains(needle) || juce::String(def.name).toLowerCase().contains(needle) ||
            juce::String(effective.name).toLowerCase().contains(needle))
            rows_.push_back(def.index);
    }
    list_.updateContent();
    list_.repaint();
}

juce::Component* ZederiaEditor::refreshComponentForRow(int row, bool, juce::Component* existing) {
    auto* component = dynamic_cast<Row*>(existing);
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        delete existing;
        return nullptr;
    }
    if (component == nullptr) {
        delete existing;
        component = new Row(processor_);
    }
    component->setIndex(rows_[static_cast<std::size_t>(row)]);
    return component;
}

void ZederiaEditor::timerCallback() {
    meter_->push(processor_.meterPeak(0), processor_.meterPeak(1));
    const int voices = processor_.activeVoices();
    status_.setText(juce::String(voices) + (voices == 1 ? " voice   " : " voices   ") + juce::String(rows_.size()) + " of " +
                        juce::String(zed::Registry::get().size()) + " parameters",
                    juce::dontSendNotification);
    if (seenGeneration_ != processor_.stateGeneration()) { // a patch was loaded: names may have changed
        seenGeneration_ = processor_.stateGeneration();
        rebuildRows();
        refreshAssetBoxes();
        processor_.engine().collectGarbage(); // frees tables the audio thread has finished with
    }
    const int first = list_.getRowContainingPosition(0, 0);
    const int visible = list_.getHeight() / juce::jmax(1, list_.getRowHeight()) + 2;
    for (int row = juce::jmax(0, first); row < first + visible && row < static_cast<int>(rows_.size()); ++row)
        if (auto* component = dynamic_cast<Row*>(list_.getComponentForRowNumber(row)))
            component->refresh();
}
