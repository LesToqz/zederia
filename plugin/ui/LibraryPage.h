// The LIBRARY page (plan 3.11): presets with facets on the left, a search box with free text and
// `field:value` filters, sortable results with a second sort, audition, multi-select for bulk
// tags, ratings, favourites, moving and pack export, Blend, pack and folder import with
// duplicates detected by sound, and module presets.
#pragma once

#include "Shell.h"

#include "zederia/content/PresetIndex.h"

#include <set>

namespace zui {

class LibraryPage final : public Page, private juce::TableListBoxModel, public juce::FileDragAndDropTarget {
public:
    explicit LibraryPage(Context& ctx);
    ~LibraryPage() override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    void structureChanged() override {}
    void saveState(nlohmann::json& state) const override;
    void loadState(const nlohmann::json& state) override;

    void rescan();
    void setQuery(const juce::String& text);
    int resultCount() const { return static_cast<int>(results_.size()); }
    const zed::PresetInfo& result(int row) const { return presets_[static_cast<std::size_t>(results_[static_cast<std::size_t>(row)])]; }
    void setSort(zed::PresetSort key, bool descending);
    void selectRow(int row, bool add = false);
    std::vector<int> selectedRows() const;
    bool loadRow(int row);
    void audition(int row);
    // Bulk edits on the selected user presets (factory presets keep their files; favourites of
    // factory presets are kept in the preferences). Return how many presets changed.
    int addTag(const juce::String& tag);
    int removeTag(const juce::String& tag);
    int setRating(int stars);
    int setFavourite(bool favourite);
    int moveTo(const juce::File& folder);
    bool exportPack(const juce::File& file, const juce::String& name);
    // Imports a .zpack file or a folder of presets into the user folder; presets whose sound is
    // already in the library are skipped. Returns how many were added; `skipped` counts duplicates.
    int importPack(const juce::File& source, int& skipped);
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    juce::File presetsFolder() const;

private:
    // Table model
    int getNumRows() override { return resultCount(); }
    void paintRowBackground(juce::Graphics& g, int row, int width, int height, bool selected) override;
    void paintCell(juce::Graphics& g, int row, int column, int width, int height, bool selected) override;
    void sortOrderChanged(int column, bool forwards) override;
    void cellDoubleClicked(int row, int column, const juce::MouseEvent&) override;
    void selectedRowsChanged(int lastRow) override;
    void returnKeyPressed(int row) override { loadRow(row); }

    void filter();
    void rebuildFacets();
    void refreshDetail();
    bool rewrite(const zed::PresetInfo& info, const std::function<void(zed::PatchMeta&)>& change);
    void showModules();

    std::vector<zed::PresetInfo> presets_;
    std::vector<int> results_;
    std::vector<std::string> problems_;
    zed::PresetSort sort_ = zed::PresetSort::Name, secondary_ = zed::PresetSort::Type;
    bool descending_ = false;
    std::set<juce::String> facetFilters_; // "type:bass", "tag:warm", "fav", "recent", "factory", "user"
    TextField search_{"Search: words, tag:warm type:bass genre:house pack:\"Name\" author:me rating:3 fav", "Search presets"};
    juce::TableListBox table_;
    Parts parts_;
    juce::Viewport facetView_;
    juce::Component facetList_;
    std::vector<std::unique_ptr<Button>> facetButtons_;
    std::vector<std::unique_ptr<Caption>> facetHeadings_;
    Button audition_{"Audition", Glyph::Speaker}, load_{"Load"}, importButton_{"Import pack", Glyph::Folder}, rescan_{"Rescan"}, duplicates_{"Find duplicates"},
        modules_{"Module presets"};
    // Detail and bulk edit
    Caption detailTitle_{"", kTextTitle, false}, detailText_{"", kTextSmall, true};
    TextField tagField_{"tag", "Tag to add or remove"};
    Button addTag_{"Add tag"}, removeTag_{"Remove tag"}, favourite_{"Favourite", Glyph::Star}, move_{"Move to folder"}, export_{"Export pack"},
        blend_{"Blend selected", Glyph::Blend}, stars_[5] = {Button{"1"}, Button{"2"}, Button{"3"}, Button{"4"}, Button{"5"}};
};

} // namespace zui
