#include "KitPage.h"

#include "Displays.h"

namespace zui {

KitPage::KitPage(Context& ctx) : Page(ctx) {
    setTitle("Kit");
    for (Card* c : {&knobs, &choices, &buttons, &curves, &displays})
        addAndMakeVisible(*c);
    const auto knob = [&](const char* id, const char* label, bool small = false) {
        auto& k = add(knobs, std::make_unique<Knob>(ctx, id, label));
        k.small = small;
        k.family = Family::Sources;
        knobRow_.push_back(&k);
    };
    knob("osc1.position", "Position");
    knob("osc1.pan", "Pan");
    knob("filter1.cutoff", "Cutoff");
    knob("osc1.level", "Level");
    knob("env2.attack", "Attack", true);
    auto& fader = add(knobs, std::make_unique<Fader>(ctx, "voice.filter_routing", "Routing"));
    fader.family = Family::Filters;
    knobRow_.push_back(&fader);
    auto& number = add(knobs, std::make_unique<NumberBox>(ctx, "osc1.unison.voices", "Voices"));
    number.costWarning = true;
    knobRow_.push_back(&number);
    knobRow_.push_back(&add(knobs, std::make_unique<NumberBox>(ctx, "osc1.semi", "Semi")));

    auto& toggle = add(choices, std::make_unique<Toggle>(ctx, "osc2.on", "Osc 2 on"));
    choiceRow_.push_back(&toggle);
    auto& power = add(choices, std::make_unique<Toggle>(ctx, "filter1.on", "Filter 1"));
    power.power = true;
    power.family = Family::Filters;
    choiceRow_.push_back(&power);
    choiceRow_.push_back(&add(choices, std::make_unique<ChoiceBox>(ctx, "filter1.type", "Filter type")));
    auto& seg = add(choices, std::make_unique<Segmented>(ctx, "osc1.engine", "Engine"));
    seg.names = {"Table", "Classic"};
    choiceRow_.push_back(&seg);
    choiceRow_.push_back(&add(choices, std::make_unique<ChoiceBox>(ctx, "lfo1.division", "Division")));

    auto& b1 = add(buttons, std::make_unique<Button>("Button"));
    auto& b2 = add(buttons, std::make_unique<Button>("Accent", Glyph::Save));
    b2.accentStyle = true;
    b2.family = Family::Sources;
    auto& b3 = add(buttons, std::make_unique<Button>("Toggled"));
    b3.setToggled(true);
    auto& icon = add(buttons, std::make_unique<Button>("", Glyph::Dice));
    icon.setTitle("Icon button");
    icon.setTooltip("Icon button");
    auto& tabs = add(buttons, std::make_unique<TabStrip>(juce::StringArray{"MAIN", "WARP", "UNISON", "PHASE"}));
    tabs.setTitle("Tabs");
    tabs.family = Family::Sources;
    auto& chip = add(buttons, std::make_unique<DragChip>(ctx, "lfo1"));
    auto& chip2 = add(buttons, std::make_unique<DragChip>(ctx, "env2"));
    auto& meter = add(buttons, std::make_unique<Meter>(ctx));
    for (juce::Component* c : std::initializer_list<juce::Component*>{&b1, &b2, &b3, &icon, &tabs, &chip, &chip2, &meter})
        buttonRow_.push_back(c);

    curve_ = &add(curves, std::make_unique<CurveEditor>(ctx, "LFO 1 shape"));
    curve_->setShape({});

    displayRow_.push_back(&add(displays, std::make_unique<WaveMini>(ctx, 0)));
    displayRow_.push_back(&add(displays, std::make_unique<WavetableView>(ctx, 0)));
    auto& flat = add(displays, std::make_unique<WavetableView>(ctx, 0));
    flat.show3d = false;
    displayRow_.push_back(&flat);
    displayRow_.push_back(&add(displays, std::make_unique<FilterResponseView>(ctx, 0)));
    displayRow_.push_back(&add(displays, std::make_unique<EnvelopeView>(ctx, 1)));
    displayRow_.push_back(&add(displays, std::make_unique<SpectrumView>(ctx)));
    displayRow_.push_back(&add(displays, std::make_unique<SignalMap>(ctx)));
}

void KitPage::paint(juce::Graphics&) {} // the chassis shows through

void KitPage::resized() {
    auto area = getLocalBounds().reduced(8);
    auto top = area.removeFromTop(110);
    knobs.setBounds(top.removeFromLeft(560));
    top.removeFromLeft(8);
    choices.setBounds(top);
    area.removeFromTop(8);
    auto mid = area.removeFromTop(150);
    buttons.setBounds(mid.removeFromLeft(560));
    mid.removeFromLeft(8);
    curves.setBounds(mid);
    area.removeFromTop(8);
    displays.setBounds(area);

    auto row = knobs.body();
    for (std::size_t i = 0; i < knobRow_.size(); ++i) {
        const int w = i < 5 ? kKnobW : i == 5 ? 150 : 60;
        auto cell = row.removeFromLeft(w);
        knobRow_[i]->setBounds(i < 5 ? cell.withHeight(kKnobH) : i == 5 ? cell.withSizeKeepingCentre(w, 22) : cell.withHeight(38));
        row.removeFromLeft(6);
    }
    row = choices.body();
    placeRow(row.removeFromTop(22), {{choiceRow_[0], 110}, {choiceRow_[1], 22}});
    row.removeFromTop(6);
    placeRow(row.removeFromTop(kChoiceH), {{choiceRow_[2], 150}, {choiceRow_[3], 130}, {choiceRow_[4], 90}}, 8);
    row = buttons.body();
    placeRow(row.removeFromTop(26), {{buttonRow_[0], 80}, {buttonRow_[1], 90}, {buttonRow_[2], 80}, {buttonRow_[3], 26}}, 6);
    row.removeFromTop(6);
    buttonRow_[4]->setBounds(row.removeFromTop(26).withWidth(320));
    row.removeFromTop(6);
    placeRow(row.removeFromTop(22), {{buttonRow_[5], 90}, {buttonRow_[6], 90}, {buttonRow_[7], 120}}, 8);
    curve_->setBounds(curves.body());
    row = displays.body();
    displayRow_[0]->setBounds(row.removeFromLeft(80).withHeight(30));
    row.removeFromLeft(8);
    const int w = (row.getWidth() - 5 * 8) / 6;
    for (std::size_t i = 1; i < displayRow_.size(); ++i) {
        displayRow_[i]->setBounds(row.removeFromLeft(w));
        row.removeFromLeft(8);
    }
}

} // namespace zui
