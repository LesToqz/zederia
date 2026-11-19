// The LIBRARY page (plan 3.11): presets with the folder tree and facets on the left (each section
// folds away), a search box with free text and `field:value` filters, sortable results with a
// second sort, a click loads a preset, multi-select for bulk tags, ratings, favourites, moving and
// pack export, Blend, pack and folder import with duplicates detected by sound, and module presets.
//
// The Presets folder (Documents\Zederia\Presets) holds one folder per kind of sound for the free
// presets that come with Zederia (808, Bass, Synth ...) and User Presets for your own, with any
// sub-folders you make (packs of your own). The folders are made when the library first opens.
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
    // Selects from code (bulk edits, tests): nothing loads. A click on a row, or the arrow keys,
    // select one preset and load it.
    void selectRow(int row, bool add = false);
    std::vector<int> selectedRows() const;
    bool loadRow(int row);
    juce::TableListBox& table() { return table_; }
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

    // --- Folders ---------------------------------------------------------------------------------------
    // The folders of the free presets that come with Zederia, then the folder for your own.
    static const juce::StringArray& defaultFolders();
    static juce::String userPresetsName() { return "User Presets"; }
    // Makes the default folders inside `presets` where they are missing.
    static void makeDefaultFolders(const juce::File& presets);
    // Every folder inside `presets`, as paths relative to it with '/' ("User Presets/My Pack"),
    // the default folders first in their order, then the others by name.
    static juce::StringArray folderPaths(const juce::File& presets);
    // The folder a typed location names inside `presets`: a relative path ("User Presets/My Pack"),
    // or a full path inside it; nothing typed means User Presets. Unsafe parts ("..") are dropped
    // and names made legal; a full path outside `presets` gives an error and juce::File().
    static juce::File resolveLocation(const juce::File& presets, const juce::String& typed, juce::String& error);

    // The folder the results are limited to ("" for all), as a path relative to Presets.
    void setFolder(const juce::String& relative);
    const juce::String& folder() const { return folderFilter_; }
    // The folder rows the tree shows now (open folders show their sub-folders).
    juce::StringArray visibleFolders() const;
    void setFolderOpen(const juce::String& relative, bool open);
    // Sections of the left column fold away: "presets", "show", "type", "tag", "genre", "pack", "author".
    void setSectionCollapsed(const juce::String& key, bool collapsed);
    bool sectionCollapsed(const juce::String& key) const { return collapsed_.count(key) > 0; }

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
    void newFolder();
    bool inFolder(const zed::PresetInfo& p) const;

    std::vector<zed::PresetInfo> presets_;
    std::vector<int> results_;
    std::vector<std::string> problems_;
    juce::StringArray folders_; // every folder the tree can show (from the disk and from the presets)
    zed::PresetSort sort_ = zed::PresetSort::Name, secondary_ = zed::PresetSort::Type;
    bool descending_ = false;
    std::set<juce::String> facetFilters_; // "type:bass", "tag:warm", "fav", "recent", "factory", "user"
    juce::String folderFilter_;
    std::set<juce::String> openFolders_{userPresetsName()};
    std::set<juce::String> collapsed_;
    bool selectingInCode_ = false; // selections made by the page itself load nothing
    TextField search_{"Search: words, tag:warm type:bass genre:house pack:\"Name\" author:me rating:3 fav", "Search presets"};
    juce::TableListBox table_;
    Parts parts_;
    juce::Viewport facetView_;
    juce::Component facetList_;
    std::vector<std::unique_ptr<Button>> facetButtons_;
    Button importButton_{"Import pack", Glyph::Folder}, newFolder_{"New folder", Glyph::Plus}, showFolder_{"Show in Explorer", Glyph::Folder},
        rescan_{"Rescan"}, duplicates_{"Find duplicates"}, modules_{"Module presets"};
    // Detail and bulk edit
    Caption detailTitle_{"", kTextTitle, false}, detailText_{"", kTextSmall, true};
    TextField tagField_{"tag", "Tag to add or remove"};
    Button addTag_{"Add tag"}, removeTag_{"Remove tag"}, favourite_{"Favourite", Glyph::Star}, move_{"Move to folder"}, export_{"Export pack"},
        blend_{"Blend selected", Glyph::Blend}, stars_[5] = {Button{"1"}, Button{"2"}, Button{"3"}, Button{"4"}, Button{"5"}};
};

} // namespace zui
