#include "ModPage.h"

#include "Audit.h"
#include "CurveEditor.h"

#include "zederia/patch/Modules.h"

#include <set>

namespace zui {

namespace {

const zed::Registry& reg() { return zed::Registry::get(); }

juce::String destName(int dest, const ZederiaProcessor& p) {
    if (dest < 0)
        return "?";
    const auto& own = reg().at(dest);
    const auto& d = p.effectiveDef(dest);
    if (&d != &own)
        for (const auto& m : reg().modules())
            if (m.id == own.module)
                return str(m.name) + " " + str(d.name);
    return str(own.name);
}

const char* const kCurveNames[5] = {"Linear", "Exp", "Log", "S", "Drawn"};

} // namespace

// --- One route -----------------------------------------------------------------------------------------

class RouteRow final : public juce::Component {
public:
    RouteRow(Context& ctx, ModPage& page, int route) : ctx_(ctx), page_(page), route_(route), amount_(ctx, reg().modAmount(route), "") {
        setTitle("Route " + juce::String(route + 1));
        amount_.family = Family::Modulators;
        amount_.labelLeft = true;
        amount_.labelledBy = "Amount";
        for (juce::Component* c : std::initializer_list<juce::Component*>{&select_, &number_, &on_, &source_, &via_, &amount_, &polarity_, &curve_,
                                                                           &dest_, &mute_, &solo_, &remove_})
            addAndMakeVisible(c);
        for (Button* b : {&on_, &mute_, &solo_})
            b->toggleRole = true;
        select_.checkbox = true; // a checkbox for bulk edits, apart from the route's own switches
        for (Button* b : {&select_, &on_, &source_, &via_, &polarity_, &curve_, &dest_, &mute_, &solo_, &remove_})
            b->textSize = kTextSmall;
        remove_.ghost = true;
        select_.onClick = [this] { page_.setSelected(route_, !select_.toggled()); };
        on_.onClick = [this] { ctx_.session.setRouteEnabled(route_, !on_.toggled()); };
        mute_.onClick = [this] { ctx_.session.setRouteMute(route_, !mute_.toggled()); };
        solo_.onClick = [this] { ctx_.session.setRouteSolo(route_, !solo_.toggled()); };
        polarity_.onClick = [this] { ctx_.session.flipPolarity(route_); };
        remove_.onClick = [this] { ctx_.session.removeRoute(route_); };
        source_.onClick = [this] {
            juce::PopupMenu menu;
            buildSourceMenu(menu, [this](const juce::String& s) { ctx_.session.setRouteSource(route_, s); });
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&source_));
        };
        via_.onClick = [this] {
            juce::PopupMenu menu;
            const auto* r = ctx_.route(route_);
            const bool invert = r != nullptr && r->route.viaInvert;
            const juce::String current = r != nullptr ? str(r->route.via) : juce::String();
            menu.addItem("No Via", [this] { ctx_.session.setRouteVia(route_, {}, false); });
            menu.addItem("Invert Via", current.isNotEmpty(), invert, [this, current, invert] { ctx_.session.setRouteVia(route_, current, !invert); });
            menu.addSeparator();
            buildSourceMenu(menu, [this, invert](const juce::String& s) { ctx_.session.setRouteVia(route_, s, invert); });
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&via_));
        };
        curve_.onClick = [this] {
            juce::PopupMenu menu;
            for (int c = 0; c < 5; ++c)
                menu.addItem(kCurveNames[c], [this, c] {
                    ctx_.session.setRouteCurve(route_, c);
                    page_.focusRoute(route_);
                });
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&curve_));
        };
        dest_.onClick = [this] {
            juce::PopupMenu menu;
            buildDestinationMenu(menu, ctx_.processor, [this](int d) { ctx_.session.setRouteDest(route_, d); });
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&dest_));
        };
        refresh();
    }

    void refresh() {
        const auto* r = ctx_.route(route_);
        if (r == nullptr)
            return;
        const auto& route = r->route;
        number_.setText(juce::String(route_ + 1));
        on_.setText(route.enabled ? "On" : "Off");
        on_.setToggled(route.enabled);
        on_.setTitle("Route " + juce::String(route_ + 1) + (route.enabled ? " on" : " off"));
        source_.setText(Context::sourceName(str(route.source)));
        source_.setTitle("Source of route " + juce::String(route_ + 1) + ": " + source_.text());
        via_.setText(route.via.empty() ? juce::String("Via: none") : (route.viaInvert ? "Via: -" : "Via: ") + Context::sourceName(str(route.via)));
        via_.setTitle(via_.text());
        polarity_.setText(route.polarity == zed::Polarity::Bi ? "+/-" : "+");
        polarity_.setTitle(route.polarity == zed::Polarity::Bi ? "Bipolar" : "Unipolar");
        curve_.setText(kCurveNames[static_cast<int>(route.curve)]);
        curve_.setTitle("Curve " + curve_.text());
        dest_.setText(destName(r->dest, ctx_.processor));
        dest_.setTitle("Destination: " + dest_.text());
        mute_.setToggled(route.mute);
        mute_.setTitle("Mute route " + juce::String(route_ + 1));
        solo_.setToggled(route.solo);
        solo_.setTitle("Solo route " + juce::String(route_ + 1));
        select_.setTitle("Select route " + juce::String(route_ + 1));
        remove_.setTitle("Remove route " + juce::String(route_ + 1));
        remove_.setTooltip("Remove this route");
        amount_.setTitle("Amount of route " + juce::String(route_ + 1));
        colour_ = r->colour;
        audible_ = ctx_.routeAudible(*r);
        repaint();
    }
    void setSelectedLook(bool selected) {
        select_.setToggled(selected);
        select_.repaint();
    }
    void resized() override {
        auto area = getLocalBounds().reduced(4, 3);
        area.removeFromLeft(4);
        placeRow(area, {{&select_, 22}, {&number_, 24}, {&on_, 36}, {&source_, 108}, {&via_, 112}, {&amount_, 190}, {&polarity_, 38},
                        {&curve_, 66}, {&dest_, 230}, {&mute_, 26}, {&solo_, 26}, {&remove_, 24}},
                 4);
    }
    void paint(juce::Graphics& g) override {
        const Theme& t = theme();
        const auto area = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(focused ? t.selection : t.panel);
        g.fillRoundedRectangle(area, 4.0f);
        g.setColour(colour_.withAlpha(audible_ ? 1.0f : 0.35f));
        g.fillRoundedRectangle(area.withWidth(3.0f).reduced(0.0f, 4.0f), 1.5f);
    }
    void mouseDown(const juce::MouseEvent&) override { page_.focusRoute(route_); }
    int route() const { return route_; }
    bool focused = false;

    static void buildDestinationMenu(juce::PopupMenu& menu, const ZederiaProcessor& processor, const std::function<void(int)>& pick) {
        zed::Patch patch;
        processor.readDocument([&](const zed::Patch& p) { patch = p; });
        for (const auto& m : reg().modules()) {
            juce::PopupMenu sub;
            for (int i = m.firstParam; i < m.firstParam + m.numParams; ++i) {
                if (!reg().at(i).modulatable || !patch.inUse(i))
                    continue;
                sub.addItem(destName(i, processor), [pick, i] { pick(i); });
            }
            if (sub.getNumItems() > 0)
                menu.addSubMenu(str(m.name), sub);
        }
    }

private:
    Context& ctx_;
    ModPage& page_;
    int route_;
    Button select_{""}, on_{"On"}, source_{"Source"}, via_{"Via"}, polarity_{"+"}, curve_{"Linear"}, dest_{"Destination"}, mute_{"M"}, solo_{"S"},
        remove_{"", Glyph::Close};
    Caption number_{"1", kTextSmall, true};
    Fader amount_;
    juce::Colour colour_;
    bool audible_ = true;
};

// --- The page ----------------------------------------------------------------------------------------

ModPage::ModPage(Context& ctx) : Page(ctx) {
    setTitle("MOD");
    for (juce::Component* c : std::initializer_list<juce::Component*>{&search_, &sort_, &add_, &replace_, &mute_, &remove_, &selectAll_, &map_, &count_,
                                                                       &detailTitle_, &detailText_})
        addAndMakeVisible(c);
    for (Button* b : {&sort_, &add_, &replace_, &mute_, &remove_, &selectAll_, &map_, &clearSearch_})
        b->textSize = kTextSmall + 0.5f;
    add_.accentStyle = true; // the one primary action of the page
    addChildComponent(clearSearch_);
    markOverlay(clearSearch_); // it sits in the empty list, under the message
    clearSearch_.setHelp("Clear search", "Show every route again.");
    clearSearch_.onClick = [this] { setSearch({}); };
    search_.onTextChange = [this] { rebuild(); };
    sort_.setHelp("Sort", "Order the routes by number, source, destination or amount.");
    sort_.onClick = [this] {
        juce::PopupMenu menu;
        menu.addItem("By number", [this] { setSort(Sort::Number); });
        menu.addItem("By source", [this] { setSort(Sort::Source); });
        menu.addItem("By destination", [this] { setSort(Sort::Destination); });
        menu.addItem("By amount", [this] { setSort(Sort::Amount); });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&sort_));
    };
    add_.setHelp("Add route", "Choose a source, then a destination. Dragging a chip onto a control does the same.");
    add_.onClick = [this] {
        juce::PopupMenu menu;
        buildSourceMenu(menu, [this](const juce::String& source) {
            juce::PopupMenu dests;
            RouteRow::buildDestinationMenu(dests, ctx_.processor, [this, source](int d) { addRoute(source, d); });
            dests.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&add_));
        });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&add_));
    };
    replace_.setHelp("Replace source", "Give every selected route a new source.");
    replace_.onClick = [this] {
        juce::PopupMenu menu;
        buildSourceMenu(menu, [this](const juce::String& s) { replaceSelectedSource(s); });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&replace_));
    };
    mute_.setHelp("Mute selected", "Mute (or unmute) every selected route.");
    mute_.onClick = [this] {
        const auto routes = selectedRoutes();
        if (routes.empty())
            return;
        const bool mute = ctx_.route(routes.front()) != nullptr && !ctx_.route(routes.front())->route.mute;
        ctx_.session.edit((mute ? "Mute " : "Unmute ") + juce::String(static_cast<int>(routes.size())) + " routes", [&](zed::Patch& p) {
            for (const int r : routes)
                if (r < static_cast<int>(p.routes.size()))
                    p.routes[static_cast<std::size_t>(r)].mute = mute;
            return true;
        });
    };
    remove_.setHelp("Remove selected", "Remove every selected route.");
    remove_.onClick = [this] {
        auto routes = selectedRoutes();
        if (routes.empty())
            return;
        std::sort(routes.rbegin(), routes.rend());
        ctx_.session.edit("Remove " + juce::String(static_cast<int>(routes.size())) + " routes", [&](zed::Patch& p) {
            for (const int r : routes)
                if (r < static_cast<int>(p.routes.size()))
                    p.removeRoute(r);
            return true;
        });
        selected_.clear();
        updateActions();
    };
    selectAll_.setHelp("Select all", "Checks every route the list shows (with a search, the routes that match it); again to clear.");
    selectAll_.onClick = [this] {
        if (allShownSelected())
            for (const int r : shown_)
                selected_.erase(r);
        else
            selected_.insert(shown_.begin(), shown_.end());
        for (auto& r : rows_)
            r->setSelectedLook(selected_.count(r->route()) > 0);
        updateActions();
    };
    map_.setHelp("Mod map", "Every connection drawn over the SYNTH page at once.");
    map_.onClick = [this] { if (onShowMap) onShowMap(); };
    viewport_.setViewedComponent(&list_, false);
    viewport_.setScrollBarsShown(true, false);
    viewport_.setTitle("Routes");
    markScrolling(list_);
    addAndMakeVisible(viewport_);
    curve_ = std::make_unique<CurveEditor>(ctx, "Route curve");
    CurveEditor::Config config;
    config.bipolar = false;
    config.fixedEnds = true;
    curve_->setConfig(config);
    curve_->onChange = [this](const zed::Shape& s) {
        if (focus_ >= 0)
            ctx_.processor.setRouteCurveLive(focus_, s);
    };
    curve_->onCommit = [this](const zed::Shape&, const juce::String& label) { ctx_.session.commit(label); };
    curve_->onCopy = [this] { ctx_.session.copyText(str(zed::copyShape(curve_->shape(), zed::ShapeKind::Curve))); };
    curve_->onPaste = [this] {
        zed::Shape shape;
        std::vector<zed::Issue> issues;
        if (focus_ >= 0 && zed::pasteShape(ctx_.session.clipboardText().toStdString(), zed::ShapeKind::Curve, shape, issues))
            ctx_.session.setRouteCurveShape(focus_, shape);
    };
    addChildComponent(*curve_);
    rebuild();
}

ModPage::~ModPage() = default;

void ModPage::setSearch(const juce::String& text) {
    search_.setText(text, false);
    rebuild();
}

void ModPage::setSort(Sort sort) {
    order_ = sort;
    static const char* names[] = {"number", "source", "destination", "amount"};
    sort_.setText(juce::String("Sort: ") + names[static_cast<int>(sort)]);
    rebuild();
}

std::vector<int> ModPage::selectedRoutes() const { return {selected_.begin(), selected_.end()}; }

void ModPage::setSelected(int route, bool selected) {
    if (selected)
        selected_.insert(route);
    else
        selected_.erase(route);
    for (auto& r : rows_)
        r->setSelectedLook(selected_.count(r->route()) > 0);
    updateActions();
}

bool ModPage::allShownSelected() const {
    if (shown_.empty())
        return false;
    for (const int r : shown_)
        if (selected_.count(r) == 0)
            return false;
    return true;
}

void ModPage::updateActions() {
    const int n = static_cast<int>(selected_.size());
    for (Button* b : {&replace_, &mute_, &remove_})
        b->setEnabledState(n > 0);
    replace_.setText(n > 1 ? "Replace source (" + juce::String(n) + ")" : juce::String("Replace source"));
    mute_.setText(n > 1 ? "Mute " + juce::String(n) + " selected" : juce::String("Mute selected"));
    remove_.setText(n > 1 ? "Remove " + juce::String(n) + " selected" : juce::String("Remove selected"));
    selectAll_.setEnabledState(!shown_.empty());
    selectAll_.setText(allShownSelected() ? "Clear selection" : "Select all");
    clearSearch_.setVisible(search_.getText().isNotEmpty() && shown_.empty() && !ctx_.routes().empty());
    resized();
}

bool ModPage::inspectorShown() const { return ctx_.route(focus_) != nullptr; }

void ModPage::replaceSelectedSource(const juce::String& source) { ctx_.session.replaceSource(selectedRoutes(), source); }

void ModPage::addRoute(const juce::String& source, int dest) {
    const int route = ctx_.session.addRoute(source, dest, 0.25);
    if (route < 0 && ctx_.notify)
        ctx_.notify("All 64 routes are in use, or that control cannot be modulated.");
    else
        focusRoute(route);
}

void ModPage::focusRoute(int route) {
    focus_ = route;
    ctx_.focusRoute = route;
    for (auto& r : rows_) {
        r->focused = r->route() == route;
        r->repaint();
    }
    const auto* r = ctx_.route(route);
    if (r == nullptr) { // no details without a route to inspect
        curve_->setVisible(false);
        detailTitle_.setText({});
        detailText_.setText(ctx_.routes().empty() ? juce::String() : juce::String("Click a route to inspect it."));
        repaint();
        return;
    }
    detailTitle_.setText("Route " + juce::String(route + 1) + ":  " + Context::sourceName(str(r->route.source)) + " > " + destName(r->dest, ctx_.processor));
    const bool drawn = r->route.curve == zed::ModCurve::Drawn;
    curve_->setVisible(drawn);
    if (drawn) {
        zed::Shape shape = r->route.curveShape;
        curve_->setShape(shape);
    }
    detailText_.setText(drawn ? "Draw how the source maps onto the destination." : "Choose Drawn as the curve to draw one; other curves are fixed shapes.");
    resized();
}

bool ModPage::reveal(const juce::String& module) {
    if (module.startsWith("mod.")) {
        const int route = module.fromFirstOccurrenceOf("mod.", false, false).getIntValue() - 1;
        focusRoute(route);
        for (auto& r : rows_)
            if (r->route() == route)
                viewport_.setViewPosition(0, r->getY());
    }
    return true;
}

void ModPage::structureChanged() {
    juce::String signature;
    for (const auto& r : ctx_.routes())
        signature << r.route.source << r.route.via << r.route.dest << (r.route.enabled ? 1 : 0) << (r.route.mute ? 1 : 0) << (r.route.solo ? 1 : 0)
                  << static_cast<int>(r.route.polarity) << static_cast<int>(r.route.curve) << (r.route.viaInvert ? 1 : 0) << ";";
    if (signature == signature_)
        return;
    signature_ = signature;
    rebuild();
}

void ModPage::rebuild() {
    ctx_.refresh();
    shown_.clear();
    const auto words = juce::StringArray::fromTokens(search_.getText().toLowerCase(), " ", "\"");
    for (const auto& r : ctx_.routes()) {
        const juce::String hay = (Context::sourceName(str(r.route.source)) + " " + Context::sourceName(str(r.route.via)) + " " + destName(r.dest, ctx_.processor) +
                                  " " + str(r.route.source) + " " + str(r.route.dest))
                                     .toLowerCase();
        bool all = true;
        for (const auto& w : words)
            all = all && (w.isEmpty() || hay.contains(w));
        if (all)
            shown_.push_back(r.index);
    }
    const auto key = [&](int i) -> juce::String {
        const auto* r = ctx_.route(i);
        if (order_ == Sort::Source) return Context::sourceName(str(r->route.source)).toLowerCase();
        if (order_ == Sort::Destination) return destName(r->dest, ctx_.processor).toLowerCase();
        return {};
    };
    if (order_ == Sort::Amount)
        std::stable_sort(shown_.begin(), shown_.end(), [&](int a, int b) { return std::abs(ctx_.routeAmount(a)) > std::abs(ctx_.routeAmount(b)); });
    else if (order_ != Sort::Number)
        std::stable_sort(shown_.begin(), shown_.end(), [&](int a, int b) { return key(a) < key(b); });
    rows_.clear();
    for (std::erase_if(selected_, [&](int r) { return r >= static_cast<int>(ctx_.routes().size()); }); const int route : shown_) {
        rows_.push_back(std::make_unique<RouteRow>(ctx_, *this, route));
        rows_.back()->setSelectedLook(selected_.count(route) > 0);
        rows_.back()->focused = route == focus_;
        list_.addAndMakeVisible(*rows_.back());
    }
    count_.setText(juce::String(static_cast<int>(shown_.size())) + " of " + juce::String(static_cast<int>(ctx_.routes().size())) + " routes, " +
                   juce::String(zed::kNumModRoutes - static_cast<int>(ctx_.routes().size())) + " free");
    layoutRows();
    focusRoute(focus_ < static_cast<int>(ctx_.routes().size()) ? focus_ : -1);
    updateActions();
}

void ModPage::layoutRows() {
    const int w = viewport_.getWidth() - viewport_.getScrollBarThickness();
    int y = 0;
    for (auto& r : rows_) {
        r->setBounds(0, y, w, 32);
        y += 34;
    }
    list_.setSize(w, juce::jmax(y, viewport_.getHeight()));
}

void ModPage::resized() {
    auto area = getLocalBounds().reduced(8);
    auto toolbar = area.removeFromTop(30);
    search_.setBounds(toolbar.removeFromLeft(300));
    toolbar.removeFromLeft(8);
    placeRow(toolbar, {{&add_, 104}, {&sort_, 130}, {&replace_, 140}, {&mute_, 130}, {&remove_, 150}, {&selectAll_, 110}, {&map_, 126}}, 6);
    area.removeFromTop(8);
    auto right = area.removeFromRight(360);
    area.removeFromRight(8);
    count_.setBounds(area.removeFromTop(16).withTrimmedLeft(8)); // inside the list's panel, in line with the column titles
    area.removeFromTop(22); // column titles
    viewport_.setBounds(area);
    clearSearch_.setBounds(juce::Rectangle<int>(130, 26).withCentre({area.getCentreX(), area.getY() + 68}));
    layoutRows();
    right = right.reduced(10, 0).withTrimmedTop(6); // inside its panel
    detailTitle_.setBounds(right.removeFromTop(20));
    detailText_.setBounds(right.removeFromTop(18));
    right.removeFromTop(8);
    curve_->setBounds(right.removeFromTop(300));
}

void ModPage::paint(juce::Graphics& g) {
    const Theme& t = theme();
    const auto list = viewport_.getBounds();
    // The list's panel holds the count and the column titles, its top in line with the detail panel's.
    drawPanel(g, list.toFloat().expanded(4.0f, 0.0f).withTop(static_cast<float>(count_.getY()) - 4.0f).withBottom(static_cast<float>(list.getBottom()) + 4.0f), t.sunken);
    auto header = juce::Rectangle<int>(list.getX() + 8, list.getY() - 22, list.getWidth(), 18);
    for (const auto& [title, w] : std::initializer_list<std::pair<const char*, int>>{
             {"", 22}, {"#", 24}, {"On", 36}, {"Source", 108}, {"Via", 112}, {"Amount", 190}, {"Pol", 38}, {"Curve", 66}, {"Destination", 230},
             {"M", 26}, {"S", 26}}) {
        drawText(g, title, header.removeFromLeft(w).toFloat(), kTextSmall, t.textDim, juce::Justification::centredLeft, true);
        header.removeFromLeft(4);
    }
    if (rows_.empty()) {
        // One message, where the routes would be: no routes yet, or none that match the search
        // (with Clear search under it).
        const juce::String message = ctx_.routes().empty() ? juce::String("No routes yet. Drag a modulator chip onto any control, or use Add route.")
                                                           : "No route matches \"" + search_.getText().trim() + "\".";
        const auto box = list.toFloat().withHeight(44.0f).translated(0.0f, 8.0f).reduced(12.0f, 0.0f);
        drawText(g, message, box, fittingSize(message, box.getWidth(), kTextBody), t.textDim, juce::Justification::centred);
    }
    // The inspector is a panel only while a route is focused.
    if (inspectorShown()) {
        const auto right = getLocalBounds().reduced(8).removeFromRight(360).withTrimmedTop(38);
        drawPanel(g, right.toFloat().withTrimmedTop(-4.0f), t.panel);
    }
}

} // namespace zui
