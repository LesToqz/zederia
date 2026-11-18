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

// A short phrase that suits a preset's type (plan 3.11: audition by an automatic phrase).
std::vector<zed::NoteSpec> phraseFor(const std::string& type) {
    const juce::String t = juce::String(type).toLowerCase();
    if (t.contains("bass"))
        return {{36, 0.0, 0.22, 0.9}, {36, 0.25, 0.22, 0.7}, {43, 0.5, 0.22, 0.8}, {41, 0.75, 0.22, 0.8}, {36, 1.0, 0.5, 0.9}};
    if (t.contains("pad") || t.contains("string") || t.contains("brass"))
        return {{48, 0.0, 2.2, 0.7}, {55, 0.0, 2.2, 0.65}, {60, 0.0, 2.2, 0.65}, {64, 0.0, 2.2, 0.6}};
    if (t.contains("lead"))
        return {{60, 0.0, 0.35, 0.8}, {62, 0.4, 0.35, 0.8}, {64, 0.8, 0.35, 0.8}, {67, 1.2, 0.8, 0.9}};
    if (t.contains("pluck") || t.contains("key") || t.contains("seq"))
        return {{60, 0.0, 0.18, 0.8}, {64, 0.2, 0.18, 0.75}, {67, 0.4, 0.18, 0.8}, {72, 0.6, 0.18, 0.8}, {67, 0.8, 0.4, 0.7}};
    if (t.contains("perc") || t.contains("drum"))
        return {{48, 0.0, 0.1, 1.0}, {48, 0.5, 0.1, 0.8}, {55, 0.75, 0.1, 0.9}};
    return {{48, 0.0, 1.2, 0.8}, {55, 0.0, 1.2, 0.7}, {64, 0.0, 1.2, 0.7}};
}

} // namespace

LibraryPage::LibraryPage(Context& ctx) : Page(ctx) {
    setTitle("LIBRARY");
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
    table_.setColour(juce::ListBox::backgroundColourId, theme().panel);
    addAndMakeVisible(table_);
    facetView_.setViewedComponent(&facetList_, false);
    facetView_.setScrollBarsShown(true, false);
    facetView_.setTitle("Facets");
    markScrolling(facetList_);
    addAndMakeVisible(facetView_);
    for (Button* b : {&audition_, &load_, &importButton_, &rescan_, &duplicates_, &modules_, &addTag_, &removeTag_, &favourite_, &move_, &export_, &blend_})
        b->textSize = kTextSmall + 0.5f;
    for (auto& s : stars_) {
        s.textSize = kTextSmall;
        s.toggleRole = true;
    }
    for (juce::Component* c : std::initializer_list<juce::Component*>{&audition_, &load_, &importButton_, &rescan_, &duplicates_, &modules_, &detailTitle_, &detailText_,
                                                                       &tagField_, &addTag_, &removeTag_, &favourite_, &move_, &export_, &blend_})
        addAndMakeVisible(c);
    for (int i = 0; i < 5; ++i) {
        addAndMakeVisible(stars_[i]);
        stars_[i].setTitle(juce::String(i + 1) + (i == 0 ? " star" : " stars"));
        stars_[i].setHelp("Rating", "Rate the selected presets.");
        stars_[i].onClick = [this, i] { setRating(i + 1); };
    }
    audition_.setHelp("Audition", "Load the selected preset and play a short phrase that suits its type. Or play the keyboard.");
    audition_.onClick = [this] { if (!selectedRows().empty()) audition(selectedRows().front()); };
    load_.setHelp("Load", "Load the selected preset (double-click or Enter does the same).");
    load_.onClick = [this] { if (!selectedRows().empty()) loadRow(selectedRows().front()); };
    importButton_.setHelp("Import pack", "Import a .zpack file or a folder of presets; sounds already in the library are skipped. Dropping one on the page works too.");
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
    rescan_.setHelp("Rescan", "Read the preset folders again.");
    rescan_.onClick = [this] { rescan(); };
    duplicates_.setHelp("Find duplicates", "Select presets whose sounds are identical.");
    duplicates_.onClick = [this] {
        const auto groups = zed::duplicatePresets(presets_);
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
    move_.setHelp("Move to folder", "Move the selected user presets into a sub-folder of your Presets folder.");
    move_.onClick = [this] {
        auto chooser = std::make_shared<juce::FileChooser>("Move presets to", presetsFolder());
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
    problems_.clear();
    const auto library = ctx_.processor.library();
    const std::string user = library.userDir().empty() ? ctx_.prefs.userContentFolder().getFullPathName().toStdString() : library.userDir();
    presets_ = zed::scanPresets(library.factoryDir(), user, &problems_);
    for (auto& p : presets_) // favourites of factory presets live in the preferences
        if (p.factory && ctx_.prefs.factoryFavourites.contains(str(p.path)))
            p.meta.favourite = true;
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

void LibraryPage::filter() {
    juce::String text = search_.getText();
    for (const auto& f : facetFilters_)
        if (f != "recent")
            text << " " << (f.contains(" ") ? f.upToFirstOccurrenceOf(":", true, false) + "\"" + f.fromFirstOccurrenceOf(":", false, false) + "\"" : f);
    const auto query = zed::parsePresetQuery(text.toStdString());
    results_.clear();
    for (int i = 0; i < static_cast<int>(presets_.size()); ++i) {
        const auto& p = presets_[static_cast<std::size_t>(i)];
        if (!zed::presetMatches(p, query))
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
}

void LibraryPage::rebuildFacets() {
    facetButtons_.clear();
    facetHeadings_.clear();
    std::vector<int> all(presets_.size());
    for (std::size_t i = 0; i < all.size(); ++i)
        all[i] = static_cast<int>(i);
    int y = 0;
    const int w = 210;
    const auto heading = [&](const juce::String& text) {
        auto c = std::make_unique<Caption>(text, kTextSmall, true);
        c->bold = true;
        c->setBounds(6, y, w - 6, 18);
        facetList_.addAndMakeVisible(*c);
        facetHeadings_.push_back(std::move(c));
        y += 20;
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
            if (facetFilters_.count(key))
                facetFilters_.erase(key);
            else
                facetFilters_.insert(key);
            rebuildFacets();
            filter();
        };
        b->setBounds(0, y, w, 22);
        y += 23;
        facetList_.addAndMakeVisible(*b);
        facetButtons_.push_back(std::move(b));
    };
    heading("SHOW");
    add("fav", "Favourites");
    add("recent", "Recent");
    add("factory", "Factory");
    add("user", "Yours");
    for (const auto& [facet, title] : {std::pair{"type", "TYPE"}, std::pair{"tag", "CHARACTER"}, std::pair{"genre", "GENRE"}, std::pair{"pack", "PACK"}, std::pair{"author", "AUTHOR"}}) {
        const auto counts = zed::presetFacet(presets_, all, facet);
        if (counts.empty())
            continue;
        y += 6;
        heading(title);
        for (const auto& [value, count] : counts)
            add(juce::String(facet) + ":" + str(value), str(value) + "  (" + juce::String(count) + ")");
    }
    facetList_.setSize(w, y + 4);
    facetList_.repaint();
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

void LibraryPage::selectedRowsChanged(int) { refreshDetail(); }

void LibraryPage::selectRow(int row, bool add) { table_.selectRow(row, false, !add); }

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

void LibraryPage::audition(int row) {
    if (!loadRow(row))
        return;
    ctx_.processor.audition(phraseFor(result(row).meta.type));
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
    const juce::File target = presetsFolder().getChildFile(juce::File::createLegalFileName(source.getFileNameWithoutExtension()));
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
    if (rows.empty()) {
        detailTitle_.setText(juce::String(resultCount()) + " of " + juce::String(static_cast<int>(presets_.size())) + " presets");
        detailText_.setText(problems_.empty() ? juce::String("Select a preset to see it; select several to edit them together.")
                                              : juce::String(static_cast<int>(problems_.size())) + " files could not be read.");
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
    auto area = getLocalBounds().reduced(8);
    auto toolbar = area.removeFromTop(30);
    search_.setBounds(toolbar.removeFromLeft(520));
    toolbar.removeFromLeft(8);
    placeRow(toolbar, {{&audition_, 100}, {&load_, 70}, {&importButton_, 118}, {&rescan_, 76}, {&duplicates_, 120}, {&modules_, 130}}, 6);
    area.removeFromTop(8);
    facetView_.setBounds(area.removeFromLeft(226).reduced(2)); // its panel (2 px around it) lines up with the results
    area.removeFromLeft(8);
    auto right = area.removeFromRight(300);
    area.removeFromRight(8);
    table_.setBounds(area);
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
    drawPanel(g, facetView_.getBounds().toFloat().expanded(2.0f), t.panel);
    const auto right = getLocalBounds().reduced(8).withTrimmedTop(38).removeFromRight(300);
    drawPanel(g, right.toFloat(), t.panel);
    if (resultCount() == 0)
        drawText(g, presets_.empty() ? "No presets yet: save one from the top bar, or import a pack." : "No preset matches.", table_.getBounds().toFloat().withHeight(80.0f).translated(0.0f, 40.0f),
                 kTextBody, t.textDim, juce::Justification::centred);
}

void LibraryPage::saveState(nlohmann::json& state) const { state["query"] = search_.getText().toStdString(); }

void LibraryPage::loadState(const nlohmann::json& state) { setQuery(juce::String(state.value("query", std::string()))); }

} // namespace zui
