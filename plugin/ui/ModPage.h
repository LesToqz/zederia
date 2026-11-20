// The MOD page (plan 3.7): every route in a list with search and sort, multi-select for bulk
// source replacement, mute and removal, each route's Via, amount, polarity, curve (with a drawn
// curve editor) and destination; and the way into the mod map.
#pragma once

#include "Shell.h"

#include <set>

namespace zui {

class RouteRow;
class CurveEditor;

class ModPage final : public Page {
public:
    explicit ModPage(Context& ctx);
    ~ModPage() override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    bool reveal(const juce::String& module) override;
    void structureChanged() override;

    enum class Sort { Number, Source, Destination, Amount };
    void setSearch(const juce::String& text);
    void setSort(Sort sort);
    std::vector<int> shownRoutes() const { return shown_; } // route numbers in display order
    std::vector<int> selectedRoutes() const;
    void setSelected(int route, bool selected);
    void focusRoute(int route);
    void replaceSelectedSource(const juce::String& source);
    void addRoute(const juce::String& source, int dest);
    std::function<void()> onShowMap; // the editor shows the mod map on the SYNTH page

    // The toolbar follows the selection: the bulk actions need checked routes, Select all needs
    // routes on show (and clears them when they are all checked).
    Button& addButton() { return add_; }
    Button& replaceButton() { return replace_; }
    Button& muteButton() { return mute_; }
    Button& removeButton() { return remove_; }
    Button& selectAllButton() { return selectAll_; }
    Button& clearSearchButton() { return clearSearch_; }
    bool inspectorShown() const; // the details of the focused route

private:
    void rebuild();
    void layoutRows();
    void updateActions();
    bool allShownSelected() const;
    void showDestinationMenu(juce::Component& target, const std::function<void(int)>& pick);

    TextField search_{"Search routes: source, Via or destination", "Search routes"};
    Button sort_{"Sort: number"}, add_{"Add route", Glyph::Plus}, replace_{"Replace source"}, mute_{"Mute selected"}, remove_{"Remove selected", Glyph::Trash},
        selectAll_{"Select all"}, map_{"Show mod map", Glyph::Map}, clearSearch_{"Clear search", Glyph::Close};
    Caption count_{"", kTextSmall, true};
    juce::Viewport viewport_;
    juce::Component list_;
    std::vector<std::unique_ptr<RouteRow>> rows_;
    std::vector<int> shown_;
    std::set<int> selected_;
    Sort order_ = Sort::Number;
    int focus_ = -1;
    juce::String signature_;
    std::unique_ptr<CurveEditor> curve_;
    Caption detailTitle_{"", kTextSmall + 0.5f, false}, detailText_{"", kTextSmall, true};
};

} // namespace zui
