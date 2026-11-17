#include "ModOverlay.h"

#include "Audit.h"

namespace zui {

namespace {

template <class T>
void collect(juce::Component& root, std::vector<T*>& out) {
    for (auto* child : root.getChildren()) {
        if (!child->isVisible())
            continue;
        if (auto* t = dynamic_cast<T*>(child))
            out.push_back(t);
        collect(*child, out);
    }
}

} // namespace

// --- ModHoverList ------------------------------------------------------------------------------------

ModHoverList::ModHoverList(Context& ctx) : ctx_(ctx) {
    setTitle("Modulation of this control");
    markOverlay(*this);
}

bool ModHoverList::showFor(ParamWidget& target) {
    target_ = &target;
    generation_ = -1;
    rebuild();
    return !rows_.empty();
}

int ModHoverList::preferredHeight() const { return 30 + static_cast<int>(rows_.size()) * 26 + 6; }

void ModHoverList::rebuild() {
    if (generation_ == ctx_.routeGeneration() && !rows_.empty())
        return;
    generation_ = ctx_.routeGeneration();
    rows_.clear();
    auto* target = target_.getComponent();
    if (target == nullptr)
        return;
    for (const auto* r : ctx_.routesTo(target->param())) {
        auto row = std::make_unique<Row>();
        row->route = r->index;
        const juce::String name = Context::sourceName(str(r->route.source));
        row->source = std::make_unique<Button>(name);
        row->source->ghost = true;
        row->source->textSize = kTextSmall;
        row->source->setTitle("Select " + name + " route");
        row->source->setHelp(name, "Select this route: Alt-drag on the control then changes its depth.");
        const int route = r->index;
        const juce::String source = str(r->route.source);
        row->source->onClick = [this, route, source] {
            ctx_.focusRoute = route;
            ctx_.selectedSource = source;
            repaint();
        };
        row->amount = std::make_unique<Fader>(ctx_, r->amountParam, "");
        row->amount->labelLeft = true;
        row->amount->family = Family::Modulators;
        row->amount->setTitle("Depth of " + name);
        row->polarity = std::make_unique<Button>(r->route.polarity == zed::Polarity::Bi ? "+/-" : "+");
        row->polarity->setTitle("Polarity of " + name + ": " + (r->route.polarity == zed::Polarity::Bi ? "bipolar" : "unipolar"));
        row->polarity->setHelp("Polarity", "Bipolar moves the control both ways around its value; unipolar one way. Shift+Alt-click on a control flips it too.");
        row->polarity->textSize = kTextSmall;
        row->polarity->onClick = [this, route] { ctx_.session.flipPolarity(route); };
        row->mute = std::make_unique<Button>("M");
        row->mute->setTitle("Mute " + name);
        row->mute->toggleRole = true;
        row->mute->setToggled(r->route.mute);
        row->mute->textSize = kTextSmall;
        row->mute->onClick = [this, route, muted = r->route.mute] { ctx_.session.setRouteMute(route, !muted); };
        row->solo = std::make_unique<Button>("S");
        row->solo->setTitle("Solo " + name);
        row->solo->toggleRole = true;
        row->solo->setToggled(r->route.solo);
        row->solo->textSize = kTextSmall;
        row->solo->onClick = [this, route, solo = r->route.solo] { ctx_.session.setRouteSolo(route, !solo); };
        row->remove = std::make_unique<Button>("", Glyph::Close);
        row->remove->setTitle("Remove " + name + " route");
        row->remove->setTooltip("Remove this route");
        row->remove->ghost = true;
        row->remove->onClick = [this, route] { ctx_.session.removeRoute(route); };
        for (juce::Component* c : std::initializer_list<juce::Component*>{row->source.get(), row->amount.get(), row->polarity.get(), row->mute.get(),
                                                                           row->solo.get(), row->remove.get()})
            addAndMakeVisible(c);
        rows_.push_back(std::move(row));
    }
    resized();
    repaint();
}

void ModHoverList::resized() {
    auto area = getLocalBounds().reduced(8, 4);
    area.removeFromTop(24);
    for (auto& row : rows_) {
        auto line = area.removeFromTop(26).reduced(0, 2);
        row->source->setBounds(line.removeFromLeft(78));
        line.removeFromLeft(4);
        row->remove->setBounds(line.removeFromRight(20));
        line.removeFromRight(2);
        row->solo->setBounds(line.removeFromRight(22));
        line.removeFromRight(2);
        row->mute->setBounds(line.removeFromRight(22));
        line.removeFromRight(2);
        row->polarity->setBounds(line.removeFromRight(32));
        line.removeFromRight(4);
        row->amount->setBounds(line);
    }
}

void ModHoverList::paint(juce::Graphics& g) {
    const Theme& t = theme();
    const auto area = getLocalBounds().toFloat();
    g.setColour(t.raised);
    g.fillRoundedRectangle(area, t.radius);
    g.setColour(t.lineStrong);
    g.drawRoundedRectangle(area.reduced(0.5f), t.radius, 1.0f);
    const juce::String title = target_ != nullptr ? target_->getTitle() : juce::String();
    drawText(g, "MODULATION  " + title, area.reduced(10.0f, 4.0f).removeFromTop(22.0f), kTextSmall, t.textDim, juce::Justification::centredLeft, true);
    float y = 28.0f;
    for (const auto& row : rows_) {
        const auto* r = ctx_.route(row->route);
        if (r != nullptr) {
            g.setColour(r->colour.withAlpha(ctx_.routeAudible(*r) ? 1.0f : 0.35f));
            g.fillRect(juce::Rectangle<float>(2.0f, y + 4.0f, 3.0f, 18.0f));
            if (r->index == ctx_.focusRoute || str(r->route.source) == ctx_.selectedSource) {
                g.setColour(r->colour.withAlpha(0.12f));
                g.fillRect(juce::Rectangle<float>(5.0f, y + 2.0f, area.getWidth() - 10.0f, 22.0f));
            }
        }
        y += 26.0f;
    }
}

// --- ModMapOverlay -----------------------------------------------------------------------------------

ModMapOverlay::ModMapOverlay(Context& ctx) : ctx_(ctx) {
    setTitle("Mod map");
    setInterceptsMouseClicks(false, false);
    markOverlay(*this);
}

void ModMapOverlay::paint(juce::Graphics& g) {
    const Theme& t = theme();
    drawn_ = 0;
    offPage_.clear();
    g.fillAll(t.bg.withAlpha(0.45f));
    if (scope == nullptr)
        return;
    std::vector<ParamWidget*> widgets;
    std::vector<DragChip*> chips;
    collect(*scope, widgets);
    collect(*scope, chips);
    const auto& reg = zed::Registry::get();
    for (const auto& r : ctx_.routes()) {
        const juce::String source = str(r.route.source);
        DragChip* chip = nullptr;
        for (auto* c : chips)
            if (c->source() == source && c->isShowing())
                chip = c;
        ParamWidget* dest = nullptr;
        for (auto* w : widgets)
            if (w->param() == r.dest && w->isShowing())
                dest = w;
        if (dest == nullptr) {
            offPage_.add(Context::sourceName(source) + " > " + (r.dest >= 0 ? str(reg.at(r.dest).name) : str(r.route.dest)));
            continue;
        }
        const auto end = getLocalPoint(dest, dest->getLocalBounds().toFloat().getCentre());
        const auto start = chip != nullptr ? getLocalPoint(chip, chip->getLocalBounds().toFloat().getCentre())
                                           : juce::Point<float>(end.x, static_cast<float>(getHeight()) - 6.0f);
        juce::Path p;
        p.startNewSubPath(start);
        const float lift = std::max(40.0f, std::abs(end.y - start.y) * 0.5f);
        p.cubicTo(start.x, start.y - lift, end.x, end.y + lift, end.x, end.y);
        const bool audible = ctx_.routeAudible(r);
        if (audible) {
            strokeGlow(g, p, r.colour, 1.6f);
        } else {
            juce::Path dashed;
            const float dashes[] = {4.0f, 4.0f};
            juce::PathStrokeType(1.4f).createDashedStroke(dashed, p, dashes, 2);
            g.setColour(r.colour.withAlpha(0.6f));
            g.fillPath(dashed);
        }
        g.setColour(r.colour);
        g.fillEllipse(juce::Rectangle<float>(7.0f, 7.0f).withCentre(end));
        g.drawEllipse(dest->getLocalBounds().toFloat().reduced(1.0f).withPosition(getLocalPoint(dest, juce::Point<float>())), 1.0f);
        ++drawn_;
    }
    const auto box = getLocalBounds().toFloat().reduced(12.0f).removeFromTop(16.0f);
    drawText(g, "MOD MAP  " + juce::String(drawn_) + " connections on this page" +
                    (offPage_.isEmpty() ? juce::String() : ", " + juce::String(offPage_.size()) + " elsewhere: " + offPage_.joinIntoString(", ")),
             box, kTextSmall + 0.5f, t.text, juce::Justification::centredLeft, true);
}

} // namespace zui
