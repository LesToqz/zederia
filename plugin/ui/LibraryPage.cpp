#include "LibraryPage.h"

#include "Audit.h"

#include "zederia/patch/Modules.h"

#include <nlohmann/json.hpp>

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
    search_.onTextChange = [this] { filter(); };
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
    for (const auto& f : facetFilters_)
        if (f != "recent")
            text << " " << (f.contains(" ") ? f.upToFirstOccurrenceOf(":", true, false) + "\"" + f.fromFirstOccurrenceOf(":", false, false) + "\"" : f);
    const auto query = zed::parsePresetQuery(text.toStdString());
    results_.clear();
    for (int i = 0; i < static_cast<int>(presets_.size()); ++i) {
        const auto& p = presets_[static_cast<std::size_t>(i)];
        if (!zed::presetMatches(p, query) || !inFolder(p))
            continue;
        if (facetFilters_.count("recent") && !ctx_.prefs.recent.contains(str(p.path)))
            continue;
        results_.push_back(i);
    }
    if (facetFilters_.count("recent") && search_.getText().isEmpty() && sort_ == zed::PresetSort::Name) {
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
    if (folderFilter_.isNotEmpty() && search_.getText().trim().isEmpty() && facetFilters_.empty())
        return Empty::Folder;
    return Empty::Search;
}

juce::String LibraryPage::emptyMessage() const { return emptyState() == Empty::None ? juce::String() : empty_->message(); }

Button* LibraryPage::emptyAction() { return emptyState() == Empty::None ? nullptr : &empty_->action; }

void LibraryPage::clearSearch() {
    search_.setText({}, false);
    facetFilters_.clear();
    rebuildFacets();
    filter();
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
    folderFilter_ = relative;
    if (relative.isNotEmpty()) // a chosen folder shows its sub-folders
        for (juce::String p = relative; p.isNotEmpty(); p = p.containsChar('/') ? p.upToLastOccurrenceOf("/", false, false) : juce::String())
            openFolders_.insert(p);
    rebuildFacets();
    filter();
}

void LibraryPage::setFolderOpen(const juce::String& relative, bool open) {
    if (open)
        openFolders_.insert(relative);
    else
        openFolders_.erase(relative);
    rebuildFacets();
}

void LibraryPage::setSectionCollapsed(const juce::String& key, bool collapsed) {
    if (collapsed)
        collapsed_.insert(key);
    else
        collapsed_.erase(key);
    rebuildFacets();
}

juce::StringArray LibraryPage::visibleFolders() const {
    juce::StringArray out;
    for (const auto& path : folders_) {
        bool shown = true;
        for (juce::String p = path; p.containsChar('/') && shown;) {
            p = p.upToLastOccurrenceOf("/", false, false);
            shown = openFolders_.count(p) > 0;
        }
        if (shown)
            out.add(path);
    }
    return out;
}

void LibraryPage::rebuildFacets() {
    facetButtons_.clear();
    std::vector<int> all(presets_.size());
    for (std::size_t i = 0; i < all.size(); ++i)
        all[i] = static_cast<int>(i);
    int y = 0;
    const int w = 210;
    // A section heading folds its section away; the handlers copy what they need first, because the
    // rebuild they start deletes the button that runs them.
    const auto heading = [&](const juce::String& key, const juce::String& text) {
        const bool open = !sectionCollapsed(key);
        auto b = std::make_unique<Button>(text, open ? Glyph::ChevronDown : Glyph::ChevronRight);
        b->ghost = true;
        b->leftAlign = true;
        b->boldText = true;
        b->dimText = true;
        b->toggleRole = true;
        b->textSize = kTextSmall;
        b->setTitle(text + (open ? " (shown)" : " (folded)"));
        b->setHelp(text, open ? "Click to fold this section away." : "Click to show this section.");
        b->onClick = [this, key, open] {
            const juce::String k = key;
            setSectionCollapsed(k, open);
        };
        b->setBounds(0, y, w, 20);
        facetList_.addAndMakeVisible(*b);
        facetButtons_.push_back(std::move(b));
        y += 22;
        return open;
    };
    const auto add = [&](const juce::String& key, const juce::String& label) {
        auto b = std::make_unique<Button>(label);
        b->ghost = true;
        b->toggleRole = true;
        b->leftAlign = true;
        b->textSize = kTextSmall + 0.5f;
        b->setToggled(facetFilters_.count(key) > 0);
        b->setTitle("Filter: " + label);
        b->onClick = [this, key] {
            LibraryPage* self = this; // the rebuild deletes this handler: nothing of it is used after
            const juce::String k = key;
            if (self->facetFilters_.count(k))
                self->facetFilters_.erase(k);
            else
                self->facetFilters_.insert(k);
            self->rebuildFacets();
            self->filter();
        };
        b->setBounds(0, y, w, 22);
        y += 23;
        facetList_.addAndMakeVisible(*b);
        facetButtons_.push_back(std::move(b));
    };

    // The Presets folder as a tree.
    if (heading("presets", "PRESETS")) {
        auto allButton = std::make_unique<Button>("All presets  (" + juce::String(static_cast<int>(presets_.size())) + ")");
        allButton->ghost = true;
        allButton->toggleRole = true;
        allButton->leftAlign = true;
        allButton->textSize = kTextSmall + 0.5f;
        allButton->setToggled(folderFilter_.isEmpty());
        allButton->setTitle("Show presets from every folder");
        allButton->onClick = [this] { setFolder({}); };
        allButton->setBounds(0, y, w, 22);
        y += 23;
        facetList_.addAndMakeVisible(*allButton);
        facetButtons_.push_back(std::move(allButton));
        for (const auto& path : visibleFolders()) {
            const int depth = depthOf(path);
            const int x = 4 + depth * 14;
            bool hasChildren = false;
            for (const auto& other : folders_)
                hasChildren = hasChildren || other.startsWithIgnoreCase(path + "/");
            const bool open = openFolders_.count(path) > 0;
            int count = 0;
            for (const auto& p : presets_) {
                const juce::String f = str(p.folder);
                count += f.equalsIgnoreCase(path) || f.startsWithIgnoreCase(path + "/") ? 1 : 0;
            }
            const juce::String name = path.fromLastOccurrenceOf("/", false, false);
            if (hasChildren) {
                auto arrow = std::make_unique<Button>("", open ? Glyph::ChevronDown : Glyph::ChevronRight);
                arrow->ghost = true;
                arrow->toggleRole = true;
                arrow->setTitle((open ? "Close " : "Open ") + name);
                arrow->setHelp(name, open ? "Hides the folders inside." : "Shows the folders inside.");
                arrow->onClick = [this, path, open] {
                    const juce::String p = path;
                    setFolderOpen(p, !open);
                };
                arrow->setBounds(x, y, 18, 22);
                facetList_.addAndMakeVisible(*arrow);
                facetButtons_.push_back(std::move(arrow));
            }
            auto b = std::make_unique<Button>(name + "  (" + juce::String(count) + ")", Glyph::Folder);
            b->ghost = true;
            b->toggleRole = true;
            b->leftAlign = true;
            b->textSize = kTextSmall + 0.5f;
            b->setToggled(folderFilter_ == path);
            b->setTitle("Folder " + path);
            b->setHelp(name, "Shows the presets in this folder and the folders inside it. Click again to show every folder.");
            b->onClick = [this, path] {
                const juce::String p = path;
                setFolder(folderFilter_ == p ? juce::String() : p);
            };
            b->setBounds(x + 18, y, w - x - 18, 22);
            y += 23;
            facetList_.addAndMakeVisible(*b);
            facetButtons_.push_back(std::move(b));
        }
    }
    y += 6;
    if (heading("show", "SHOW")) {
        add("fav", "Favourites");
        add("recent", "Recent");
        add("factory", "Factory");
        add("user", "Yours");
    }
    for (const auto& [facet, title] : {std::pair{"type", "TYPE"}, std::pair{"tag", "CHARACTER"}, std::pair{"genre", "GENRE"}, std::pair{"pack", "PACK"}, std::pair{"author", "AUTHOR"}}) {
        const auto counts = zed::presetFacet(presets_, all, facet);
        if (counts.empty())
            continue;
        y += 6;
        if (!heading(facet, title))
            continue;
        for (const auto& [value, count] : counts)
            add(juce::String(facet) + ":" + str(value), str(value) + "  (" + juce::String(count) + ")");
    }
    facetList_.setSize(w, y + 4);
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

void LibraryPage::paintRowBackground(juce::Graphics& g, int, int width, int height, bool selected) {
    if (selected) {
        g.setColour(theme().selection);
        g.fillRect(0, 0, width, height);
    }
}

void LibraryPage::paintCell(juce::Graphics& g, int row, int column, int width, int height, bool) {
    if (row < 0 || row >= resultCount())
        return;
    const Theme& t = theme();
    const auto& p = result(row);
    const auto box = juce::Rectangle<float>(6.0f, 0.0f, static_cast<float>(width) - 10.0f, static_cast<float>(height));
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
    case Rating: text = p.meta.rating > 0 ? juce::String::repeatedString("*", p.meta.rating) : juce::String("-"); break;
    case Modified: text = juce::Time(p.fileTime * 1000).formatted("%Y-%m-%d"); break;
    case Origin: text = p.factory ? "Factory" : "Yours"; break;
    default: break;
    }
    drawText(g, text, box, fittingSize(text, box.getWidth(), kTextSmall + 0.5f), column == Name ? t.text : t.textDim);
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
    search_.setBounds(toolbar.removeFromLeft(470));
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
    state["open_folders"] = nlohmann::json::array();
    for (const auto& f : openFolders_)
        state["open_folders"].push_back(f.toStdString());
    state["folded"] = nlohmann::json::array();
    for (const auto& c : collapsed_)
        state["folded"].push_back(c.toStdString());
}

void LibraryPage::loadState(const nlohmann::json& state) {
    const auto strings = [&state](const char* key, std::set<juce::String>& into) {
        if (!state.contains(key) || !state[key].is_array())
            return;
        into.clear();
        for (const auto& v : state[key])
            if (v.is_string())
                into.insert(juce::String(v.get<std::string>()));
    };
    strings("open_folders", openFolders_);
    strings("folded", collapsed_);
    const juce::String folder(state.value("folder", std::string()));
    folderFilter_ = folders_.contains(folder, true) ? folder : juce::String();
    rebuildFacets();
    setQuery(juce::String(state.value("query", std::string())));
}

} // namespace zui
