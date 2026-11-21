#include "LibraryPage.h"

#include "Audit.h"

#include "zederia/patch/Modules.h"

#include <nlohmann/json.hpp>

#include <map>
#include <numeric>

namespace zui {

namespace {

enum Column { Fav = 1, Name, Type, Pack, Author, Rating, Modified, Origin };

zed::PresetSort sortFor(int column) {
    switch (column) {
    case Type: return zed::PresetSort::Type;
    case Pack: return zed::PresetSort::Pack;
    case Author: return zed::PresetSort::Author;
    case Rating: return zed::PresetSort::Rating;
    case Modified: return zed::PresetSort::Modified;
    case Origin: return zed::PresetSort::Origin;
    default: return zed::PresetSort::Name;
    }
}

// Where a folder sorts: the free presets' folders in their order, then other folders by name, then
// User Presets last.
int folderRank(const juce::String& path) {
    const juce::String top = path.upToFirstOccurrenceOf("/", false, false);
    if (top.equalsIgnoreCase(LibraryPage::userPresetsName()))
        return 1000;
    const int i = LibraryPage::defaultFolders().indexOf(top, true);
    return i >= 0 ? i : 500;
}

void sortFolders(juce::StringArray& paths) {
    std::sort(paths.begin(), paths.end(), [](const juce::String& a, const juce::String& b) {
        const int ra = folderRank(a), rb = folderRank(b);
        if (ra != rb)
            return ra < rb;
        return a.compareNatural(b, false) < 0;
    });
}

void addFolderPath(juce::StringArray& paths, const juce::String& path) {
    juce::String p = path.replaceCharacter('\\', '/').trimCharactersAtStart("/").trimCharactersAtEnd("/");
    while (p.isNotEmpty()) { // the folder and every folder above it
        if (!paths.contains(p, true))
            paths.add(p);
        if (!p.containsChar('/'))
            break;
        p = p.upToLastOccurrenceOf("/", false, false);
    }
}

int depthOf(const juce::String& path) { return path.length() - path.removeCharacters("/").length(); }

bool inUserPresets(const juce::String& path) {
    return path.upToFirstOccurrenceOf("/", false, false).equalsIgnoreCase(LibraryPage::userPresetsName());
}

} // namespace

// --- Folders -------------------------------------------------------------------------------------------

const juce::StringArray& LibraryPage::defaultFolders() {
    // The kinds of sound the free presets that come with Zederia are sorted into (empty until the
    // factory presets are made), then the folder for your own presets.
    static const juce::StringArray folders = [] {
        juce::StringArray f{"808", "Bass", "Synth", "Lead", "Pad", "Pluck", "Keys", "Arp", "FX"};
        f.add(userPresetsName());
        return f;
    }();
    return folders;
}

void LibraryPage::makeDefaultFolders(const juce::File& presets) {
    if (presets == juce::File())
        return;
    for (const auto& name : defaultFolders()) {
        const juce::File folder = presets.getChildFile(name);
        if (!folder.isDirectory())
            folder.createDirectory();
    }
}

juce::StringArray LibraryPage::folderPaths(const juce::File& presets) {
    juce::StringArray paths;
    if (!presets.isDirectory())
        return paths;
    for (const auto& dir : presets.findChildFiles(juce::File::findDirectories, true))
        if (!dir.isHidden())
            addFolderPath(paths, dir.getRelativePathFrom(presets));
    sortFolders(paths);
    return paths;
}

juce::File LibraryPage::resolveLocation(const juce::File& presets, const juce::String& typed, juce::String& error) {
    juce::String text = typed.trim().replaceCharacter('\\', '/');
    if (text.isEmpty())
        return presets.getChildFile(userPresetsName());
    const bool full = (text.length() > 1 && text[1] == ':') || text.startsWith("//");
    if (full) {
        const juce::File chosen(text.replaceCharacter('/', '\\'));
        if (chosen == presets)
            return presets;
        if (!chosen.isAChildOf(presets)) {
            error = "Choose a folder inside " + presets.getFullPathName() + ": the library shows the presets there.";
            return {};
        }
        text = chosen.getRelativePathFrom(presets).replaceCharacter('\\', '/');
    }
    juce::StringArray parts = juce::StringArray::fromTokens(text, "/", "");
    for (auto& part : parts)
        part = part.trim();
    parts.removeEmptyStrings();
    // "Zederia/Presets/User Presets" and "Presets/User Presets" name the same folder as "User Presets".
    if (parts.size() >= 2 && parts[0].equalsIgnoreCase(presets.getParentDirectory().getFileName()) && parts[1].equalsIgnoreCase(presets.getFileName()))
        parts.removeRange(0, 2);
    else if (!parts.isEmpty() && parts[0].equalsIgnoreCase(presets.getFileName()))
        parts.remove(0);
    juce::File folder = presets;
    for (auto part : parts) {
        if (part == "." || part == "..")
            continue; // never above the Presets folder
        part = juce::File::createLegalFileName(part).trim();
        while (part.endsWithChar('.'))
            part = part.dropLastCharacters(1).trim();
        if (part.isNotEmpty())
            folder = folder.getChildFile(part);
    }
    return folder;
}

// --- The empty results -----------------------------------------------------------------------------------

// In front of the list while it has no rows: what is empty, what to do about it and the action
// that does it, beside the message. A screen reader reads the message as the view's name.
class LibraryPage::EmptyView final : public Widget {
public:
    EmptyView() {
        setAccessible(true);
        markOverlay(*this); // it sits over the empty list
        action.textSize = kTextSmall + 0.5f;
        addAndMakeVisible(action);
    }
    void set(const juce::String& title, const juce::String& text, const juce::String& actionText, Glyph glyph, std::function<void()> onAction) {
        title_ = title;
        text_ = text;
        setTitle(title + ". " + text);
        action.setText(actionText);
        action.glyph = glyph;
        action.onClick = std::move(onAction);
        resized();
        repaint();
    }
    void resized() override {
        const int w = static_cast<int>(textWidth(action.text(), kTextSmall + 0.5f)) + 44;
        action.setBounds(juce::Rectangle<int>(juce::jmin(w, getWidth()), 26).withCentre({getWidth() / 2, 70}));
    }
    void draw(juce::Graphics& g) override {
        const Theme& t = theme();
        const auto area = getLocalBounds().toFloat();
        drawText(g, title_, area.withHeight(22.0f).translated(0.0f, 4.0f), kTextTitle, t.text, juce::Justification::centred, true);
        drawText(g, text_, area.withHeight(18.0f).translated(0.0f, 28.0f), fittingSize(text_, area.getWidth(), kTextBody), t.textDim, juce::Justification::centred);
    }
    bool hasVisibleLabel() const override { return true; }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override {
        return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::staticText);
    }
    juce::String message() const { return title_ + ". " + text_; }
    Button action{"Import pack", Glyph::Folder};

private:
    juce::String title_, text_;
};

// --- The page ------------------------------------------------------------------------------------------

LibraryPage::LibraryPage(Context& ctx) : Page(ctx) {
    setTitle("LIBRARY");
    empty_ = std::make_unique<EmptyView>();
    addChildComponent(*empty_);
    addAndMakeVisible(search_);
    const juce::String searchHelp = "Search by name or enter filters such as tag:warm, type:bass, genre:house, pack:\"My Pack\", author:me, rating:3 or fav.";
    search_.setDescription(searchHelp);
    if (ctx_.prefs.tooltips)
        search_.setTooltip(searchHelp);
    search_.onTextChange = [this] { filter(); };
    addChildComponent(clearScope_);
    clearScope_.leftAlign = true;
    clearScope_.textSize = kTextSmall + 0.5f;
    clearScope_.onClick = [this] {
        setFolder({});
        if (isShowing())
            search_.grabKeyboardFocus();
    };
    auto& header = table_.getHeader();
    const int columnFlags = juce::TableHeaderComponent::defaultFlags;
    header.addColumn("Fav", Fav, 36, 30, 40, columnFlags);
    header.addColumn("Name", Name, 210, 80, 400, columnFlags);
    header.addColumn("Type", Type, 80, 50, 140, columnFlags);
    header.addColumn("Pack", Pack, 110, 50, 200, columnFlags);
    header.addColumn("Author", Author, 100, 50, 200, columnFlags);
    header.addColumn("Rating", Rating, 64, 50, 90, columnFlags);
    header.addColumn("Modified", Modified, 96, 60, 140, columnFlags);
    header.addColumn("Origin", Origin, 64, 50, 90, columnFlags);
    header.setSortColumnId(Name, true);
    table_.setModel(this);
    table_.setRowHeight(24);
    table_.setMultipleSelectionEnabled(true);
    table_.setTitle("Presets");
    table_.setHelpText("Click a preset to load it. Ctrl or Shift selects several for tags, ratings, moving and export.");
    table_.setColour(juce::ListBox::backgroundColourId, theme().sunken);
    addAndMakeVisible(table_);
    facetView_.setViewedComponent(&facetList_, false);
    facetView_.setScrollBarsShown(true, false);
    facetView_.setTitle("Folders and filters");
    markScrolling(facetList_);
    addAndMakeVisible(facetView_);
    for (Button* b : {&importButton_, &newFolder_, &showFolder_, &rescan_, &duplicates_, &modules_, &addTag_, &removeTag_, &favourite_, &move_, &export_, &blend_})
        b->textSize = kTextSmall + 0.5f;
    for (auto& s : stars_) {
        s.textSize = kTextSmall;
        s.toggleRole = true;
    }
    for (juce::Component* c : std::initializer_list<juce::Component*>{&importButton_, &newFolder_, &showFolder_, &rescan_, &duplicates_, &modules_, &detailTitle_,
                                                                       &detailText_, &tagField_, &addTag_, &removeTag_, &favourite_, &move_, &export_, &blend_})
        addAndMakeVisible(c);
    for (int i = 0; i < 5; ++i) {
        addAndMakeVisible(stars_[i]);
        stars_[i].setTitle(juce::String(i + 1) + (i == 0 ? " star" : " stars"));
        stars_[i].setHelp("Rating", "Rate the selected presets.");
        stars_[i].onClick = [this, i] { setRating(i + 1); };
    }
    importButton_.setHelp("Import pack", "Import a .zpack file or a folder of presets into User Presets; sounds already in the library are skipped. Dropping one on the page works too.");
    importButton_.onClick = [this] {
        auto chooser = std::make_shared<juce::FileChooser>("Import a pack or a folder of presets", juce::File::getSpecialLocation(juce::File::userDesktopDirectory), "*.zpack");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectDirectories,
                             [this, chooser](const juce::FileChooser& fc) {
                                 int skipped = 0;
                                 const int added = importPack(fc.getResult(), skipped);
                                 if (ctx_.notify)
                                     ctx_.notify(juce::String(added) + " presets imported, " + juce::String(skipped) + " duplicates skipped.");
                             });
    };
    newFolder_.setHelp("New folder", "Makes a folder inside the selected folder (inside User Presets when none is selected), for a pack of your own.");
    newFolder_.onClick = [this] { newFolder(); };
    showFolder_.setHelp("Show in Explorer", "Opens the selected folder (or the Presets folder) in Explorer.");
    showFolder_.onClick = [this] {
        const juce::File chosen = presetsFolder().getChildFile(folderFilter_);
        (chosen.isDirectory() ? chosen : presetsFolder()).startAsProcess();
    };
    rescan_.setHelp("Rescan", "Read the preset folders again.");
    rescan_.onClick = [this] { rescan(); };
    duplicates_.setHelp("Find duplicates", "Select presets whose sounds are identical.");
    duplicates_.onClick = [this] {
        const auto groups = zed::duplicatePresets(presets_);
        const juce::ScopedValueSetter<bool> quiet(selectingInCode_, true);
        table_.deselectAllRows();
        int found = 0;
        for (const auto& group : groups)
            for (const int p : group)
                for (int r = 0; r < resultCount(); ++r)
                    if (results_[static_cast<std::size_t>(r)] == p) {
                        table_.selectRow(r, false, false);
                        ++found;
                    }
        if (ctx_.notify)
            ctx_.notify(groups.empty() ? juce::String("No duplicates.") : juce::String(static_cast<int>(groups.size())) + " groups of identical sounds; " + juce::String(found) + " presets selected.");
    };
    modules_.setHelp("Module presets", "Save one module (an oscillator, filter, envelope, LFO, effect, arpeggiator pattern or clip) as a preset, or load one into a module of the same kind.");
    modules_.onClick = [this] { showModules(); };
    tagField_.onReturnKey = [this] { addTag(tagField_.getText()); };
    addTag_.onClick = [this] { addTag(tagField_.getText()); };
    removeTag_.onClick = [this] { removeTag(tagField_.getText()); };
    favourite_.toggleRole = true;
    favourite_.setHelp("Favourite", "Mark the selected presets as favourites.");
    favourite_.onClick = [this] { setFavourite(!favourite_.toggled()); };
    move_.setHelp("Move to folder", "Move the selected user presets into a folder inside your Presets folder.");
    move_.onClick = [this] {
        auto chooser = std::make_shared<juce::FileChooser>("Move presets to", presetsFolder().getChildFile(userPresetsName()));
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this, chooser](const juce::FileChooser& fc) {
            if (fc.getResult().isDirectory())
                moveTo(fc.getResult());
        });
    };
    export_.setHelp("Export pack", "Write the selected presets as one .zpack file to share.");
    export_.onClick = [this] {
        auto chooser = std::make_shared<juce::FileChooser>("Export a pack", ctx_.prefs.userContentFolder().getChildFile("Packs").getChildFile("My Pack.zpack"), "*.zpack");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this, chooser](const juce::FileChooser& fc) {
            if (fc.getResult() != juce::File())
                exportPack(fc.getResult().withFileExtension("zpack"), fc.getResult().getFileNameWithoutExtension());
        });
    };
    blend_.setHelp("Blend", "Build a new sound from modules of the selected presets (each module comes whole from one of them).");
    blend_.onClick = [this] {
        juce::Array<juce::File> files;
        for (const int r : selectedRows())
            files.add(juce::File(str(result(r).path)));
        juce::String error;
        if (!ctx_.session.blend(files, error) && ctx_.notify)
            ctx_.notify(error);
    };
    rescan();
}

LibraryPage::~LibraryPage() { table_.setModel(nullptr); }

juce::File LibraryPage::presetsFolder() const { return ctx_.userFolder().getChildFile("Presets"); }

void LibraryPage::rescan() {
    const juce::ScopedValueSetter<bool> quiet(selectingInCode_, true);
    problems_.clear();
    makeDefaultFolders(presetsFolder());
    const auto library = ctx_.processor.library();
    const std::string user = library.userDir().empty() ? ctx_.prefs.userContentFolder().getFullPathName().toStdString() : library.userDir();
    presets_ = zed::scanPresets(library.factoryDir(), user, &problems_);
    for (auto& p : presets_) // favourites of factory presets live in the preferences
        if (p.factory && ctx_.prefs.factoryFavourites.contains(str(p.path)))
            p.meta.favourite = true;
    // The tree: the folders on disk (empty ones too) and the folders the presets are in (factory
    // presets of a kind share the folder of that kind).
    folders_ = folderPaths(presetsFolder());
    for (const auto& p : presets_)
        addFolderPath(folders_, str(p.folder));
    sortFolders(folders_);
    if (folderFilter_.isNotEmpty() && !folders_.contains(folderFilter_, true))
        folderFilter_.clear();
    rebuildFacets();
    filter();
}

void LibraryPage::setQuery(const juce::String& text) {
    search_.setText(text, false);
    filter();
}

void LibraryPage::setSort(zed::PresetSort key, bool descending) {
    if (key != sort_)
        secondary_ = sort_; // the previous key becomes the second sort
    sort_ = key;
    descending_ = descending;
    filter();
}

bool LibraryPage::inFolder(const zed::PresetInfo& p) const {
    if (folderFilter_.isEmpty())
        return true;
    const juce::String f = str(p.folder);
    return f.equalsIgnoreCase(folderFilter_) || f.startsWithIgnoreCase(folderFilter_ + "/");
}

void LibraryPage::filter() {
    const juce::ScopedValueSetter<bool> quiet(selectingInCode_, true);
    juce::String text = search_.getText();
    if (facetFilter_.isNotEmpty() && facetFilter_ != "recent")
        text << " " << (facetFilter_.contains(" ") ? facetFilter_.upToFirstOccurrenceOf(":", true, false) + "\"" + facetFilter_.fromFirstOccurrenceOf(":", false, false) + "\"" : facetFilter_);
    const auto query = zed::parsePresetQuery(text.toStdString());
    results_.clear();
    for (int i = 0; i < static_cast<int>(presets_.size()); ++i) {
        const auto& p = presets_[static_cast<std::size_t>(i)];
        if (!zed::presetMatches(p, query) || !inFolder(p))
            continue;
        if (facetFilter_ == "recent" && !ctx_.prefs.recent.contains(str(p.path)))
            continue;
        results_.push_back(i);
    }
    if (facetFilter_ == "recent" && search_.getText().isEmpty() && sort_ == zed::PresetSort::Name) {
        std::sort(results_.begin(), results_.end(), [&](int a, int b) {
            return ctx_.prefs.recent.indexOf(str(presets_[static_cast<std::size_t>(a)].path)) < ctx_.prefs.recent.indexOf(str(presets_[static_cast<std::size_t>(b)].path));
        });
    } else {
        zed::sortPresets(presets_, results_, sort_, descending_, secondary_);
    }
    table_.updateContent();
    table_.repaint();
    refreshDetail();
    refreshEmpty();
}

LibraryPage::Empty LibraryPage::emptyState() const {
    if (resultCount() > 0)
        return Empty::None;
    if (presets_.empty())
        return Empty::Library;
    if (folderFilter_.isNotEmpty() && search_.getText().trim().isEmpty())
        return Empty::Folder;
    return Empty::Search;
}

juce::String LibraryPage::emptyMessage() const { return emptyState() == Empty::None ? juce::String() : empty_->message(); }

Button* LibraryPage::emptyAction() { return emptyState() == Empty::None ? nullptr : &empty_->action; }

void LibraryPage::clearSearch() {
    search_.setText({}, false);
    setScope(folderFilter_, {});
}

void LibraryPage::refreshEmpty() {
    const Empty state = emptyState();
    switch (state) {
    case Empty::Library:
        empty_->set("No presets yet", "Save a sound from the top bar, or bring in presets from a pack or a folder.", "Import pack", Glyph::Folder,
                    [this] { if (importButton_.onClick) importButton_.onClick(); });
        break;
    case Empty::Folder:
        empty_->set("This folder is empty", "Save a preset into " + folderFilter_.fromLastOccurrenceOf("/", false, false) + " from the top bar (Save).",
                    "Show all presets", Glyph::None, [this] { setFolder({}); });
        break;
    case Empty::Search:
        empty_->set("No preset matches", juce::String("Nothing in ") + (folderFilter_.isEmpty() ? juce::String("the library") : folderFilter_.fromLastOccurrenceOf("/", false, false)) +
                        " matches the search and filters.", "Clear search", Glyph::Close, [this] { clearSearch(); });
        break;
    case Empty::None: break;
    }
    empty_->setVisible(state != Empty::None);
    if (state != Empty::None)
        empty_->toFront(false);
}

bool LibraryPage::selectionToolsShown() const { return favourite_.isVisible(); }

void LibraryPage::setFolder(const juce::String& relative) {
    if (relative.isNotEmpty()) {
        openSections_.insert(inUserPresets(relative) ? "user_presets" : "presets");
        for (juce::String p = relative; p.isNotEmpty(); p = p.containsChar('/') ? p.upToLastOccurrenceOf("/", false, false) : juce::String())
            openFolders_.insert(p.toLowerCase());
    }
    setScope(relative, {});
}

void LibraryPage::setScope(juce::String folder, juce::String facet) {
    const juce::ScopedValueSetter<bool> quiet(selectingInCode_, true);
    if (folderFilter_ != folder || facetFilter_ != facet)
        table_.deselectAllRows();
    folderFilter_ = std::move(folder);
    facetFilter_ = std::move(facet);
    rebuildFacets();
    filter();
}

void LibraryPage::setFolderOpen(const juce::String& relative, bool open) {
    if (relative.equalsIgnoreCase(userPresetsName())) {
        setSectionCollapsed("user_presets", !open);
        return;
    }
    if (open)
        openFolders_.insert(relative.toLowerCase());
    else
        openFolders_.erase(relative.toLowerCase());
    rebuildFacets();
}

void LibraryPage::setSectionCollapsed(const juce::String& key, bool collapsed) {
    if (collapsed)
        openSections_.erase(key);
    else
        openSections_.insert(key);
    rebuildFacets();
}

juce::StringArray LibraryPage::visibleFolders() const {
    juce::StringArray out;
    for (const auto& path : folders_) {
        if (sectionCollapsed(inUserPresets(path) ? "user_presets" : "presets"))
            continue;
        bool shown = true;
        for (juce::String p = path; p.containsChar('/') && shown;) {
            p = p.upToLastOccurrenceOf("/", false, false);
            shown = p.equalsIgnoreCase(userPresetsName()) || openFolders_.count(p.toLowerCase()) > 0;
        }
        if (shown)
            out.add(path);
    }
    return out;
}

void LibraryPage::rebuildFacets() {
    const auto* focused = juce::Component::getCurrentlyFocusedComponent();
    const auto focusId = focused && facetList_.isParentOf(focused) ? focused->getComponentID() : juce::String();
    facetRows_.clear();
    int y = 0;
    constexpr int w = 210;

    const auto button = [&](const juce::String& id, const juce::String& label, Glyph glyph, int x, int width, int height) -> Button& {
        auto b = std::make_unique<Button>(label, glyph);
        b->setComponentID(id);
        b->ghost = true;
        b->leftAlign = true;
        b->textSize = kTextSmall + 0.5f;
        b->setBounds(x, y, width, height);
        auto& row = *b;
        facetList_.addAndMakeVisible(row);
        facetRows_.push_back(std::move(b));
        return row;
    };
    const auto heading = [&](const juce::String& key, const juce::String& label) {
        const bool open = !sectionCollapsed(key);
        auto& b = button("section/" + key, label, open ? Glyph::ChevronDown : Glyph::ChevronRight, 0, w, 26);
        b.boldText = b.dimText = true;
        b.textSize = kTextSmall;
        b.setTitle(label + (open ? " (shown)" : " (folded)"));
        b.setHelp(label, open ? "Collapse this section." : "Expand this section.");
        b.onClick = [this, key, open] { setSectionCollapsed(key, open); };
        y += 28;
        return open;
    };
    const auto addFacet = [&](const juce::String& key, const juce::String& label) {
        auto& b = button("filter/" + key, label, Glyph::None, 8, w - 8, 22);
        b.toggleRole = true;
        b.setToggled(facetFilter_ == key);
        b.setTitle("Filter: " + label);
        b.onClick = [this, key] { setScope({}, facetFilter_ == key ? juce::String() : key); };
        y += 23;
    };

    const auto visible = visibleFolders();
    std::map<juce::String, int> counts;
    std::set<juce::String> parents;
    if (!visible.isEmpty()) {
        for (const auto& item : presets_) {
            for (auto path = str(item.folder).toLowerCase(); path.isNotEmpty(); path = path.upToLastOccurrenceOf("/", false, false)) {
                ++counts[path];
                if (!path.containsChar('/'))
                    break;
            }
        }
        for (const auto& path : folders_)
            if (path.containsChar('/'))
                parents.insert(path.upToLastOccurrenceOf("/", false, false).toLowerCase());
    }
    const auto addFolders = [&](bool user) {
        for (const auto& path : visible) {
            if (inUserPresets(path) != user)
                continue;
            const bool root = path.equalsIgnoreCase(userPresetsName());
            const auto name = root ? juce::String("All user presets") : path.fromLastOccurrenceOf("/", false, false);
            const auto key = path.toLowerCase();
            const int x = 4 + 14 * juce::jlimit(0, 4, depthOf(path) - (user ? 1 : 0));
            if (!root && parents.count(key)) {
                const bool open = openFolders_.count(key) > 0;
                auto& arrow = button("disclose/" + path, {}, open ? Glyph::ChevronDown : Glyph::ChevronRight, x, 18, 22);
                arrow.setTitle((open ? "Close " : "Open ") + name);
                arrow.setHelp(name, open ? "Hide subfolders." : "Show subfolders.");
                arrow.onClick = [this, path, open] { setFolderOpen(path, !open); };
            }
            const int indent = root ? 8 : x + 18;
            auto& b = button("folder/" + path, name + "  (" + juce::String(counts[key]) + ")", root ? Glyph::None : Glyph::Folder, indent, w - indent, 22);
            b.toggleRole = true;
            b.setToggled(folderFilter_.equalsIgnoreCase(path));
            b.setTitle("Folder " + path);
            b.setHelp(name, "Show presets in this folder and its subfolders. Click again to show all presets.");
            b.onClick = [this, path] { setFolder(folderFilter_.equalsIgnoreCase(path) ? juce::String() : path); };
            y += 23;
        }
    };
    if (heading("presets", "PRESETS")) {
        auto& b = button("all", "All presets  (" + juce::String(static_cast<int>(presets_.size())) + ")", Glyph::None, 8, w - 8, 22);
        b.toggleRole = true;
        b.setToggled(folderFilter_.isEmpty() && facetFilter_.isEmpty());
        b.setTitle("Show presets from every folder");
        b.onClick = [this] { setFolder({}); };
        y += 23;
        addFolders(false);
    }
    if (heading("user_presets", "USER PRESETS"))
        addFolders(true);
    if (heading("show", "SHOW")) {
        addFacet("fav", "Favourites");
        addFacet("recent", "Recent");
        addFacet("factory", "Factory");
        addFacet("user", "Yours");
    }
    std::vector<int> all;
    for (const auto& [facet, title] : {std::pair{"type", "TYPE"}, std::pair{"tag", "CHARACTER"}, std::pair{"genre", "GENRE"}}) {
        if (!heading(facet, title))
            continue;
        if (all.empty()) {
            all.resize(presets_.size());
            std::iota(all.begin(), all.end(), 0);
        }
        const auto values = zed::presetFacet(presets_, all, facet);
        for (const auto& [value, count] : values)
            addFacet(juce::String(facet) + ":" + str(value), str(value) + "  (" + juce::String(count) + ")");
        if (values.empty()) {
            auto hint = std::make_unique<Caption>("No " + juce::String(title).toLowerCase() + " assigned");
            hint->setBounds(14, y, w - 14, 22);
            facetList_.addAndMakeVisible(*hint);
            facetRows_.push_back(std::move(hint));
            y += 23;
        }
    }
    facetList_.setSize(w, y + 4);
    const auto scope = folderFilter_.isNotEmpty() ? folderFilter_ : facetFilter_;
    if (scope.isNotEmpty()) {
        auto label = folderFilter_.isNotEmpty() ? folderFilter_.fromLastOccurrenceOf("/", false, false) : facetFilter_;
        if (facetFilter_ == "fav") label = "Favourites";
        if (facetFilter_ == "recent") label = "Recent";
        if (facetFilter_ == "factory") label = "Factory";
        if (facetFilter_ == "user") label = "Yours";
        clearScope_.setText(label);
        clearScope_.setTitle("Clear sidebar selection: " + scope);
        clearScope_.setHelp(scope, "Clear this selection and keep the search text.");
    }
    if (clearScope_.isVisible() != scope.isNotEmpty()) {
        clearScope_.setVisible(scope.isNotEmpty());
        resized();
    }
    if (focusId.isNotEmpty())
        if (auto* replacement = facetList_.findChildWithID(focusId))
            replacement->grabKeyboardFocus();
    facetList_.repaint();
}

void LibraryPage::newFolder() {
    const juce::File base = folderFilter_.isNotEmpty() && presetsFolder().getChildFile(folderFilter_).isDirectory()
                                ? presetsFolder().getChildFile(folderFilter_)
                                : presetsFolder().getChildFile(userPresetsName());
    auto dialog = std::make_shared<juce::AlertWindow>("New folder", "Inside " + base.getFullPathName(), juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("name", "My Pack", "Name");
    dialog->addButton("Make", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<LibraryPage> safe(this);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create([safe, dialog, base](int result) {
        if (result != 1 || safe == nullptr)
            return;
        const juce::String typed = dialog->getTextEditorContents("name").trim();
        juce::String error;
        const juce::File folder = typed.isEmpty() ? juce::File() : resolveLocation(base, typed, error);
        if (folder == juce::File() || folder == base) {
            if (safe->ctx_.notify)
                safe->ctx_.notify(error.isNotEmpty() ? error : juce::String("Give the folder a name."));
            return;
        }
        if (!folder.createDirectory()) {
            if (safe->ctx_.notify)
                safe->ctx_.notify("Could not make " + folder.getFullPathName());
            return;
        }
        safe->rescan();
        safe->setFolder(folder.getRelativePathFrom(safe->presetsFolder()).replaceCharacter('\\', '/'));
    }), false);
}

void LibraryPage::paintRowBackground(juce::Graphics& g, int row, int width, int height, bool selected) {
    const auto& t = theme();
    if (row >= resultCount()) return;
    g.setColour(selected ? t.selection : row % 2 == 0 ? t.sunken : t.display);
    g.fillRect(0, 0, width, height);
}

void LibraryPage::paintCell(juce::Graphics& g, int row, int column, int width, int height, bool selected) {
    if (row < 0 || row >= resultCount())
        return;
    const Theme& t = theme();
    const auto& p = result(row);
    auto box = juce::Rectangle<float>(6.0f, 0.0f, static_cast<float>(width) - 10.0f, static_cast<float>(height));
    juce::String text;
    switch (column) {
    case Fav:
        if (p.meta.favourite) {
            g.setColour(t.warning);
            g.fillPath(glyphPath(Glyph::StarFilled, box.withSizeKeepingCentre(12.0f, 12.0f)));
        }
        return;
    case Name: text = str(p.meta.name); break;
    case Type: text = str(p.meta.type); break;
    case Pack: text = str(p.meta.pack.empty() ? p.folder : p.meta.pack); break;
    case Author: text = str(p.meta.author); break;
    case Rating:
        if (p.meta.rating > 0) {
            g.setColour(t.lamp);
            g.fillPath(glyphPath(Glyph::StarFilled, box.removeFromLeft(12.0f).withSizeKeepingCentre(11.0f, 11.0f)));
            box.removeFromLeft(3.0f);
            text = juce::String(p.meta.rating) + "/5";
        } else {
            text = juce::String::fromUTF8("\xe2\x80\x94");
        }
        break;
    case Modified: text = juce::Time(p.fileTime * 1000).formatted("%Y-%m-%d"); break;
    case Origin: text = p.factory ? "Factory" : "Yours"; break;
    default: break;
    }
    const bool bold = column == Name && selected;
    drawText(g, text, box, fittingSize(text, box.getWidth(), column == Name ? kTextBody : kTextSmall + 0.5f, bold),
             column == Name ? t.text : t.textDim, juce::Justification::centredLeft, bold);
}

void LibraryPage::sortOrderChanged(int column, bool forwards) { setSort(sortFor(column), !forwards); }

void LibraryPage::cellDoubleClicked(int row, int, const juce::MouseEvent&) { loadRow(row); }

void LibraryPage::selectedRowsChanged(int) {
    refreshDetail();
    if (selectingInCode_)
        return;
    // A click (or an arrow key) that leaves one preset selected loads it. Ctrl and Shift build a
    // selection for bulk edits without loading anything.
    const auto rows = selectedRows();
    const auto mods = juce::ModifierKeys::getCurrentModifiers();
    if (rows.size() == 1 && !mods.isCommandDown() && !mods.isShiftDown() && !mods.isPopupMenu())
        loadRow(rows.front());
}

void LibraryPage::selectRow(int row, bool add) {
    const juce::ScopedValueSetter<bool> quiet(selectingInCode_, true);
    table_.selectRow(row, false, !add);
}

std::vector<int> LibraryPage::selectedRows() const {
    std::vector<int> out;
    const auto set = table_.getSelectedRows();
    for (int i = 0; i < set.size(); ++i)
        if (set[i] >= 0 && set[i] < resultCount())
            out.push_back(set[i]);
    return out;
}

bool LibraryPage::loadRow(int row) {
    if (row < 0 || row >= resultCount())
        return false;
    juce::String error;
    const bool ok = ctx_.session.loadPresetFile(juce::File(str(result(row).path)), error);
    if (!ok && ctx_.notify)
        ctx_.notify(error);
    return ok;
}

bool LibraryPage::rewrite(const zed::PresetInfo& info, const std::function<void(zed::PatchMeta&)>& change) {
    if (info.factory)
        return false;
    juce::MemoryBlock data;
    const juce::File file(str(info.path));
    if (!file.loadFileAsData(data))
        return false;
    zed::LoadResult loaded = zed::loadPatch(std::string_view(static_cast<const char*>(data.getData()), data.getSize()));
    if (!loaded.ok())
        return false;
    change(loaded.patch.meta);
    const std::string text = zed::savePatch(loaded.patch);
    return file.replaceWithData(text.data(), text.size());
}

int LibraryPage::addTag(const juce::String& tag) {
    const std::string t = tag.trim().toLowerCase().toStdString();
    if (t.empty())
        return 0;
    int n = 0;
    for (const int r : selectedRows())
        n += rewrite(result(r), [&](zed::PatchMeta& m) {
            if (std::find(m.tags.begin(), m.tags.end(), t) == m.tags.end())
                m.tags.push_back(t);
        }) ? 1 : 0;
    rescan();
    return n;
}

int LibraryPage::removeTag(const juce::String& tag) {
    const std::string t = tag.trim().toLowerCase().toStdString();
    int n = 0;
    for (const int r : selectedRows())
        n += rewrite(result(r), [&](zed::PatchMeta& m) { std::erase(m.tags, t); }) ? 1 : 0;
    rescan();
    return n;
}

int LibraryPage::setRating(int stars) {
    int n = 0;
    for (const int r : selectedRows())
        n += rewrite(result(r), [&](zed::PatchMeta& m) { m.rating = juce::jlimit(0, 5, stars); }) ? 1 : 0;
    if (n == 0 && !selectedRows().empty() && ctx_.notify)
        ctx_.notify("Factory presets keep their ratings; rate your own presets or copies.");
    rescan();
    return n;
}

int LibraryPage::setFavourite(bool favourite) {
    int n = 0;
    for (const int r : selectedRows()) {
        const auto& p = result(r);
        if (p.factory) {
            if (favourite)
                ctx_.prefs.factoryFavourites.addIfNotAlreadyThere(str(p.path));
            else
                ctx_.prefs.factoryFavourites.removeString(str(p.path));
            ++n;
        } else {
            n += rewrite(p, [&](zed::PatchMeta& m) { m.favourite = favourite; }) ? 1 : 0;
        }
    }
    Prefs::get().save();
    rescan();
    return n;
}

int LibraryPage::moveTo(const juce::File& folder) {
    int n = 0;
    folder.createDirectory();
    for (const int r : selectedRows()) {
        const auto& p = result(r);
        if (p.factory)
            continue;
        const juce::File file(str(p.path));
        n += file.moveFileTo(folder.getChildFile(file.getFileName())) ? 1 : 0;
    }
    rescan();
    return n;
}

bool LibraryPage::exportPack(const juce::File& file, const juce::String& name) {
    juce::ZipFile::Builder zip;
    nlohmann::json manifest = nlohmann::json::object();
    manifest["format"] = "zederia.pack";
    manifest["version"] = 1;
    manifest["name"] = name.toStdString();
    manifest["presets"] = nlohmann::json::array();
    for (const int r : selectedRows()) {
        const auto& p = result(r);
        const juce::File source(str(p.path));
        zip.addFile(source, 6, "Presets/" + source.getFileName());
        manifest["presets"].push_back(("Presets/" + source.getFileName()).toStdString());
    }
    if (manifest["presets"].empty())
        return false;
    const juce::File temp = juce::File::createTempFile("manifest.json");
    temp.replaceWithText(juce::String::fromUTF8(manifest.dump(2).c_str()));
    zip.addFile(temp, 6, "manifest.json");
    file.deleteFile();
    juce::FileOutputStream out(file);
    const bool ok = out.openedOk() && zip.writeToStream(out, nullptr);
    out.flush();
    temp.deleteFile();
    if (ok && ctx_.notify)
        ctx_.notify("Pack written: " + file.getFullPathName());
    return ok;
}

int LibraryPage::importPack(const juce::File& source, int& skipped) {
    skipped = 0;
    std::set<std::uint64_t> known;
    for (const auto& p : presets_)
        known.insert(p.soundHash);
    // A pack becomes a folder of your own presets: User Presets/<pack name>.
    const juce::File target = presetsFolder().getChildFile(userPresetsName()).getChildFile(juce::File::createLegalFileName(source.getFileNameWithoutExtension()));
    int added = 0;
    const auto take = [&](const std::string& bytes, const juce::String& fileName) {
        const zed::LoadResult loaded = zed::loadPatch(bytes);
        if (!loaded.ok())
            return;
        const auto hash = zed::soundHash(loaded.patch);
        if (known.count(hash)) {
            ++skipped;
            return;
        }
        known.insert(hash);
        target.createDirectory();
        juce::File file = target.getChildFile(juce::File::createLegalFileName(fileName));
        if (file.existsAsFile())
            file = file.getNonexistentSibling();
        if (file.replaceWithData(bytes.data(), bytes.size()))
            ++added;
    };
    if (source.isDirectory()) {
        for (const auto& f : source.findChildFiles(juce::File::findFiles, true, "*.zpreset"))
            take(f.loadFileAsString().toStdString(), f.getFileName());
    } else if (source.hasFileExtension("zpack") || source.hasFileExtension("zip")) {
        juce::ZipFile zip(source);
        for (int i = 0; i < zip.getNumEntries(); ++i) {
            const auto* entry = zip.getEntry(i);
            if (entry == nullptr || !entry->filename.endsWithIgnoreCase(".zpreset") || entry->uncompressedSize > static_cast<juce::int64>(zed::kMaxPatchBytes))
                continue;
            std::unique_ptr<juce::InputStream> in(zip.createStreamForEntry(i));
            if (in == nullptr)
                continue;
            take(in->readEntireStreamAsString().toStdString(), entry->filename.fromLastOccurrenceOf("/", false, false));
        }
    } else if (source.hasFileExtension("zpreset")) {
        take(source.loadFileAsString().toStdString(), source.getFileName());
    }
    rescan();
    return added;
}

bool LibraryPage::isInterestedInFileDrag(const juce::StringArray& files) {
    for (const auto& f : files)
        if (juce::File(f).isDirectory() || f.endsWithIgnoreCase(".zpack") || f.endsWithIgnoreCase(".zpreset"))
            return true;
    return false;
}

void LibraryPage::filesDropped(const juce::StringArray& files, int, int) {
    int total = 0, skippedTotal = 0;
    for (const auto& f : files) {
        int skipped = 0;
        total += importPack(juce::File(f), skipped);
        skippedTotal += skipped;
    }
    if (ctx_.notify)
        ctx_.notify(juce::String(total) + " presets imported, " + juce::String(skippedTotal) + " duplicates skipped.");
}

void LibraryPage::refreshDetail() {
    const auto rows = selectedRows();
    // The tools that edit presets show only while there are presets to edit.
    for (juce::Component* c : std::initializer_list<juce::Component*>{&tagField_, &addTag_, &removeTag_, &favourite_, &move_, &export_, &blend_})
        c->setVisible(!rows.empty());
    for (auto& s : stars_)
        s.setVisible(!rows.empty());
    if (rows.empty()) {
        detailTitle_.setText(juce::String(resultCount()) + " of " + juce::String(static_cast<int>(presets_.size())) + " presets");
        detailText_.setText(!problems_.empty() ? juce::String(static_cast<int>(problems_.size())) + " files could not be read."
                            : resultCount() == 0 ? juce::String()
                                                 : juce::String("Click a preset to load it; Ctrl or Shift selects several to edit together."));
        favourite_.setToggled(false);
        for (auto& s : stars_)
            s.setToggled(false);
        return;
    }
    const auto& p = result(rows.front());
    if (rows.size() == 1) {
        detailTitle_.setText(str(p.meta.name));
        juce::StringArray tags;
        for (const auto& t : p.meta.tags)
            tags.add(str(t));
        detailText_.setText((p.meta.type.empty() ? juce::String("No type") : str(p.meta.type)) + (p.meta.author.empty() ? juce::String() : ", by " + str(p.meta.author)) +
                            (tags.isEmpty() ? juce::String() : ".  Tags: " + tags.joinIntoString(", ")));
    } else {
        detailTitle_.setText(juce::String(static_cast<int>(rows.size())) + " presets selected");
        detailText_.setText("Tags, ratings, favourite, moving and export apply to all of them.");
    }
    favourite_.setToggled(p.meta.favourite);
    for (int i = 0; i < 5; ++i)
        stars_[i].setToggled(i < p.meta.rating);
}

void LibraryPage::showModules() {
    juce::PopupMenu menu;
    const juce::File folder = ctx_.prefs.userContentFolder().getChildFile("Modules");
    juce::PopupMenu save, load;
    static const char* const modules[] = {"osc1", "osc2", "osc3", "sub", "texture", "filter1", "filter2", "env1", "env2", "env3", "env4", "lfo1", "lfo2", "lfo3",
                                          "lfo4", "lfo5", "lfo6", "lfo7", "lfo8", "flux1", "flux2", "voice", "arp", "chord", "fx"};
    for (const char* m : modules) {
        const juce::String id(m);
        save.addItem(id, [this, id, folder] {
            const std::string text = zed::copyModule(ctx_.processor.snapshot(), id.toStdString());
            const juce::String cls = str(zed::moduleClass(id.toStdString()));
            auto chooser = std::make_shared<juce::FileChooser>("Save module preset", folder.getChildFile(str(zed::moduleClassName(cls.toStdString()))).getChildFile(id + ".zmodule"), "*.zmodule");
            chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [chooser, text](const juce::FileChooser& fc) {
                if (fc.getResult() != juce::File()) {
                    fc.getResult().getParentDirectory().createDirectory();
                    fc.getResult().replaceWithText(str(text));
                }
            });
        });
    }
    // Saved module presets, offered for every module of the same kind.
    for (const auto& file : folder.findChildFiles(juce::File::findFiles, true, "*.zmodule")) {
        const std::string text = file.loadFileAsString().toStdString();
        const std::string cls = zed::moduleDocumentClass(text);
        juce::PopupMenu into;
        for (const char* m : modules)
            if (zed::moduleClass(m) == cls)
                into.addItem(juce::String("Into ") + m, [this, text, m, file] {
                    std::vector<zed::Issue> issues;
                    ctx_.session.edit("Load " + file.getFileNameWithoutExtension() + " into " + m, [&](zed::Patch& p) { return zed::pasteModule(text, p, m, issues); });
                    if (!issues.empty() && ctx_.notify)
                        ctx_.notify(str(issues.front().message));
                });
        if (into.getNumItems() > 0)
            load.addSubMenu(file.getFileNameWithoutExtension() + "  (" + str(zed::moduleClassName(cls)) + ")", into);
    }
    menu.addSubMenu("Save a module as a preset", save);
    menu.addSubMenu("Load a module preset", load, load.getNumItems() > 0);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&modules_));
}

void LibraryPage::resized() {
    auto area = getLocalBounds();
    auto toolbar = area.removeFromTop(42).reduced(8, 6);
    auto searchArea = toolbar.removeFromLeft(470);
    if (clearScope_.isVisible()) {
        clearScope_.setBounds(searchArea.removeFromRight(152));
        searchArea.removeFromRight(4);
    }
    search_.setBounds(searchArea);
    toolbar.removeFromLeft(8);
    placeRow(toolbar, {{&importButton_, 112}, {&newFolder_, 104}, {&showFolder_, 136}, {&rescan_, 76}, {&duplicates_, 120}, {&modules_, 130}}, 6);
    facetView_.setBounds(area.removeFromLeft(226).reduced(6, 3));
    auto right = area.removeFromRight(300);
    table_.setBounds(area);
    empty_->setBounds(area.withTrimmedTop(48).withHeight(104).reduced(16, 0));
    right = right.reduced(10, 8);
    detailTitle_.setBounds(right.removeFromTop(22));
    detailText_.setBounds(right.removeFromTop(36));
    right.removeFromTop(8);
    auto starRow = right.removeFromTop(24);
    for (auto& s : stars_) {
        s.setBounds(starRow.removeFromLeft(30));
        starRow.removeFromLeft(4);
    }
    right.removeFromTop(8);
    favourite_.setBounds(right.removeFromTop(26));
    right.removeFromTop(8);
    tagField_.setBounds(right.removeFromTop(26));
    right.removeFromTop(4);
    auto tagRow = right.removeFromTop(26);
    addTag_.setBounds(tagRow.removeFromLeft(tagRow.getWidth() / 2 - 2));
    tagRow.removeFromLeft(4);
    removeTag_.setBounds(tagRow);
    right.removeFromTop(8);
    move_.setBounds(right.removeFromTop(26));
    right.removeFromTop(4);
    export_.setBounds(right.removeFromTop(26));
    right.removeFromTop(4);
    blend_.setBounds(right.removeFromTop(26));
}

void LibraryPage::paint(juce::Graphics& g) {
    const Theme& t = theme();
    drawPlate(g, getLocalBounds().withHeight(table_.getY()).toFloat());
    drawPanel(g, facetView_.getBounds().expanded(6, 3).toFloat(), t.panel);
    const auto right = getLocalBounds().withTrimmedTop(table_.getY()).removeFromRight(300);
    drawPanel(g, right.toFloat(), t.panel);
    // An empty list says so in front of it (EmptyView): the list paints its own background.
}

void LibraryPage::saveState(nlohmann::json& state) const {
    state["query"] = search_.getText().toStdString();
    state["folder"] = folderFilter_.toStdString();
}

void LibraryPage::loadState(const nlohmann::json& state) {
    openSections_.clear();
    openFolders_.clear();
    facetFilter_.clear();
    const juce::String folder(state.value("folder", std::string()));
    folderFilter_ = folders_.contains(folder, true) ? folder : juce::String();
    rebuildFacets();
    setQuery(juce::String(state.value("query", std::string())));
}

} // namespace zui
