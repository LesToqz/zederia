// zederia-cli ui-snapshot: editor pages rendered offscreen to PNG at chosen scales and themes,
// with the automatic interface checks of plan 6.1. Built when the plug-in is part of the build.
#include "CliUtil.h"

#if ZED_HAVE_UI
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ui/Audit.h"
#include "ui/FxPage.h"
#include "zederia/params/Registry.h"

#include <juce_events/juce_events.h>

#include <cctype>
#include <functional>
#include <map>
#include <set>

namespace cli {

namespace {

const char* const kPages[] = {"synth", "fx", "mod", "table", "play", "library", "agent", "kit"};

std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == ',') {
            if (!current.empty())
                out.push_back(current);
            current.clear();
        } else if (c != ' ') {
            current += c;
        }
    }
    if (!current.empty())
        out.push_back(current);
    return out;
}

} // namespace

int cmdUiSnapshot(const Args& args) {
    juce::ScopedJuceInitialiser_GUI gui;
    const bool trace = args.has("trace");
    const auto step = [&](const char* what) {
        if (trace) {
            std::fprintf(stderr, "[ui-snapshot] %s\n", what);
            std::fflush(stderr);
        }
    };
    step("start");
    // Default preferences unless a settings file is named: snapshots must not depend on whoever runs them.
    Prefs::useFile(args.has("settings") ? juce::File(juce::String::fromUTF8(args.get("settings").c_str()))
                                        : juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("zederia-snapshot-settings-none.json"));
    step("prefs");
    std::vector<int> pages;
    for (const auto& name : split(args.get("page", "all"))) {
        if (name == "all") {
            for (int p = 0; p < 7; ++p)
                pages.push_back(p);
            continue;
        }
        const auto it = std::find_if(std::begin(kPages), std::end(kPages), [&](const char* p) { return name == p; });
        if (it == std::end(kPages)) {
            std::fprintf(stderr, "error: unknown page \"%s\"\n", name.c_str());
            return kUsage;
        }
        pages.push_back(static_cast<int>(it - std::begin(kPages)));
    }
    std::vector<int> scales;
    for (const auto& s : split(args.get("scale", "100,150,200"))) {
        double v = 0.0;
        if (!toNumber(s, v) || v < 70.0 || v > 300.0) {
            std::fprintf(stderr, "error: --scale takes percentages from 70 to 300\n");
            return kUsage;
        }
        scales.push_back(static_cast<int>(v));
    }
    step("pages");
    juce::StringArray themes;
    const juce::String themeArg = juce::String::fromUTF8(args.get("theme", "Graphite").c_str());
    if (themeArg.equalsIgnoreCase("all"))
        themes = zui::Theme::names();
    else
        for (const auto& t : zui::Theme::names())
            if (t.equalsIgnoreCase(themeArg))
                themes.add(t);
    if (themes.isEmpty()) {
        std::fprintf(stderr, "error: unknown theme; use %s or all\n", zui::Theme::names().joinIntoString(", ").toRawUTF8());
        return kUsage;
    }
    step("themes");
    const juce::File outDir = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(args.get("out", "ui-snapshots").c_str()));
    outDir.createDirectory();

    step("processor");
    ZederiaProcessor processor;
    step("processor made");
    processor.setPlayConfigDetails(0, 2, 48000.0, 512);
    processor.prepareToPlay(48000.0, 512);
    if (args.has("content") || args.has("user"))
        processor.setContentFolders(args.get("content"), args.get("user"));
    if (args.has("patch")) {
        std::vector<std::uint8_t> bytes;
        std::string error;
        if (!zed::readFile(args.get("patch"), bytes, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return kFailed;
        }
        const auto loaded = processor.loadPatchJson(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        if (!loaded.ok()) {
            std::fprintf(stderr, "error: the patch has errors\n");
            return kFailed;
        }
        processor.session().resetHistory("Opened");
    }
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    const auto run = [&](int blocks) {
        for (int b = 0; b < blocks; ++b) {
            buffer.clear();
            processor.processBlock(buffer, midi);
            midi.clear();
        }
    };
    if (args.has("play")) {
        for (const auto& note : split(args.get("play"))) {
            double key = 0.0;
            if (toNumber(note, key))
                midi.addEvent(juce::MidiMessage::noteOn(1, static_cast<int>(key), 0.8f), 0);
        }
        run(40);
    }

    step("editor");
    std::unique_ptr<juce::AudioProcessorEditor> base(processor.createEditor());
    step("editor made");
    auto* editor = dynamic_cast<ZederiaEditor*>(base.get());
    if (editor == nullptr)
        return kFailed;
    const bool check = args.has("audit");
    int findings = 0;
    std::vector<zui::Finding> themeFindings;
    for (const auto& themeName : themes) {
        editor->context().prefs.theme = themeName; // the editor takes its theme from the preferences (not saved here)
        editor->applyTheme();
        if (check)
            for (auto& f : zui::checkTheme(zui::Theme::current()))
                themeFindings.push_back(f);
        for (const int page : pages) {
            for (const int scale : scales) {
                editor->showPage(page);
                editor->setScalePercent(scale);
                for (int frame = 0; frame < 3; ++frame) { // let the displays catch up with the engine
                    run(1);
                    editor->tickFrame();
                }
                step("render");
                std::vector<zui::Finding> list;
                const juce::Image image = zui::renderSnapshot(*editor, check ? &list : nullptr);
                if (check)
                    for (auto& f : zui::checkLayout(*editor))
                        list.push_back(f);
                juce::String name = juce::String(kPages[page]) + "_" + juce::String(scale);
                if (themeName != "Graphite")
                    name << "_" << themeName.toLowerCase();
                const juce::File file = outDir.getChildFile(name + ".png");
                if (!zui::writePng(image, file)) {
                    std::fprintf(stderr, "error: could not write %s\n", file.getFullPathName().toRawUTF8());
                    return kFailed;
                }
                std::printf("%s  %dx%d  %d finding(s)\n", file.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight(),
                            static_cast<int>(list.size()));
                for (const auto& f : list)
                    std::printf("  %s: %s: %s\n", f.kind.toRawUTF8(), f.where.toRawUTF8(), f.detail.toRawUTF8());
                findings += static_cast<int>(list.size());
            }
        }
    }
    for (const auto& f : themeFindings)
        std::printf("  %s: %s: %s\n", f.kind.toRawUTF8(), f.where.toRawUTF8(), f.detail.toRawUTF8());
    findings += static_cast<int>(themeFindings.size());
    base.reset();
    if (check)
        std::printf("%d finding(s) in total\n", findings);
    return check && findings > 0 ? kFailed : kOk;
}

// --- ui-help ----------------------------------------------------------------------------------------

namespace {

// One help-strip text and the controls that show it: what the strip says when the pointer is over them.
struct HelpEntry {
    juce::StringArray titles;
    juce::String text;
};

// A card (panel) of a page and the help texts of the controls on it, in reading order.
struct HelpSection {
    juce::String title;
    juce::StringArray sameAs; // later cards whose controls say exactly the same
    std::vector<HelpEntry> entries;
};

juce::String markdownText(juce::String s) {
    return s.replace("|", "\\|").replace("\r", " ").replace("\n", " ").trim();
}

// A title with its first number taken out ("Osc 2 Level" -> "Osc # Level"), and that number.
juce::String numberless(const juce::String& title, int* number = nullptr) {
    const int start = title.indexOfAnyOf("0123456789");
    if (start < 0)
        return title;
    int end = start;
    while (end < title.length() && juce::CharacterFunctions::isDigit(title[end]))
        ++end;
    if (number != nullptr)
        *number = title.substring(start, end).getIntValue();
    return title.substring(0, start) + "#" + title.substring(end);
}

// "Env 1, Env 2, Env 3, LFO 1, LFO 2" -> "Env 1 to 3, LFO 1 to 2"; "Shape: Free, Shape: Line" -> "Shape: Free, Line".
juce::String joinTitles(const juce::StringArray& titles) {
    juce::StringArray parts;
    for (int i = 0; i < titles.size();) {
        int first = 0;
        const juce::String key = numberless(titles[i], &first);
        int j = i + 1, last = first;
        for (int n = 0; j < titles.size() && key != titles[i] && numberless(titles[j], &n) == key && n == last + 1; ++j)
            last = n;
        if (j - i > 1)
            parts.add(key.replace("#", juce::String(first) + " to " + juce::String(last)));
        else
            parts.add(titles[i]);
        i = j;
    }
    // A shared "Name: " prefix is said once.
    const int colon = parts.size() > 1 ? parts[0].indexOf(": ") : -1;
    if (colon > 0) {
        const juce::String prefix = parts[0].substring(0, colon + 2);
        bool shared = true;
        for (const auto& p : parts)
            shared = shared && p.startsWith(prefix);
        if (shared) {
            juce::StringArray rest;
            for (const auto& p : parts)
                rest.add(p.substring(prefix.length()));
            return prefix + rest.joinIntoString(", ");
        }
    }
    return parts.joinIntoString(", ");
}

// A help text found on screen, where it was, and in which state of the page (`pass`: a tab, a
// modulator or a lane shown).
struct FoundHelp {
    juce::Rectangle<int> where;
    juce::String card;
    juce::Rectangle<int> cardWhere;
    HelpEntry entry;
    int pass = 0;
    const juce::Component* component = nullptr;
};

bool skipped(juce::Component* c, const std::vector<juce::Component*>& skip) { return std::find(skip.begin(), skip.end(), c) != skip.end(); }

// Controls of an effect slot (fx.a1.p3) belong under the effect's name, in slot order, with the
// slot's prefix ("FX A1 ") taken off their titles. Set while the FX page is read.
std::function<bool(const zui::Widget&, juce::String& card, int& order, juce::String& prefix)> effectCardOf;

// The visible help texts below `root`. `skip` holds components that belong elsewhere (the pages,
// when the frame around them is read).
void gatherHelp(juce::Component& root, juce::Component& reference, const std::vector<juce::Component*>& skip, int pass, std::vector<FoundHelp>& found) {
    using Found = FoundHelp;
    std::function<void(juce::Component&, zui::Card*)> walk = [&](juce::Component& c, zui::Card* card) {
        for (auto* child : c.getChildren()) {
            if (!child->isVisible() || skipped(child, skip))
                continue;
            auto* asCard = dynamic_cast<zui::Card*>(child);
            zui::Card* within = asCard != nullptr && asCard->titleText().isNotEmpty() ? asCard : card;
            if (auto* w = dynamic_cast<zui::Widget*>(child); w != nullptr && asCard == nullptr && w->helpText.isNotEmpty()) {
                Found f;
                f.component = child;
                f.where = reference.getLocalArea(child, child->getLocalBounds());
                if (within != nullptr) {
                    f.card = within->titleText();
                    f.cardWhere = reference.getLocalArea(within, within->getLocalBounds());
                }
                juce::String title = markdownText(w->helpTitle), text = markdownText(w->helpText);
                // A tool without a title says what it is first ("Drawing shape: Free. Drag across the wave.").
                if (title.isEmpty() && text.indexOf(": ") > 0 && text.indexOf(". ") > text.indexOf(": ")) {
                    title = text.upToFirstOccurrenceOf(". ", false, false);
                    text = text.fromFirstOccurrenceOf(". ", false, false);
                }
                juce::String effect, prefix;
                int order = 0;
                if (effectCardOf && effectCardOf(*w, effect, order, prefix)) {
                    f.card = effect;
                    f.cardWhere = {0, 1000000 + order, 1, 1}; // after the page's own cards, in slot order
                    if (title.startsWith(prefix))
                        title = title.substring(prefix.length());
                }
                f.entry = {juce::StringArray(title), text};
                f.pass = pass;
                found.push_back(f);
            }
            walk(*child, within);
        }
    };
    walk(root, nullptr);
}

// The module tab strips below `root` that are showing (not the page tabs).
std::vector<zui::TabStrip*> tabStrips(juce::Component& root, const std::vector<juce::Component*>& skip) {
    std::vector<zui::TabStrip*> out;
    std::function<void(juce::Component&)> walk = [&](juce::Component& c) {
        for (auto* child : c.getChildren()) {
            if (!child->isVisible() || skipped(child, skip))
                continue;
            if (auto* t = dynamic_cast<zui::TabStrip*>(child); t != nullptr && !t->large)
                out.push_back(t);
            walk(*child);
        }
    };
    walk(root);
    return out;
}

// The help texts as the page shows them, and with each other tab of each tab strip on it.
void gatherWithTabs(ZederiaEditor& editor, juce::Component& root, const std::vector<juce::Component*>& skip, int& pass, std::vector<FoundHelp>& found) {
    const auto settle = [&] {
        for (int frame = 0; frame < 2; ++frame)
            editor.tickFrame();
    };
    settle();
    gatherHelp(root, editor, skip, pass++, found);
    for (auto* strip : tabStrips(root, skip)) {
        const int shown = strip->selected();
        for (int i = 0; i < strip->count(); ++i) {
            if (i == shown || !strip->tab(i).isVisible())
                continue;
            strip->setSelected(i, true);
            settle();
            gatherHelp(root, editor, skip, pass++, found);
        }
        strip->setSelected(shown, true);
        settle();
    }
}

// Sections in reading order: cards top to bottom and left to right; on a card, what showed first,
// then what each further tab or state added.
std::vector<HelpSection> sectionsOf(std::vector<FoundHelp> found) {
    using Found = FoundHelp;
    const auto before = [](juce::Rectangle<int> a, juce::Rectangle<int> b) {
        if (std::abs(a.getY() - b.getY()) > 12)
            return a.getY() < b.getY();
        return a.getX() < b.getX();
    };
    std::stable_sort(found.begin(), found.end(), [&](const Found& a, const Found& b) {
        if (a.cardWhere != b.cardWhere)
            return before(a.cardWhere, b.cardWhere);
        if (a.pass != b.pass)
            return a.pass < b.pass;
        return before(a.where, b.where);
    });
    std::vector<HelpSection> sections;
    // A control read again in a later state of the page (a part that stays while tabs change) counts
    // once. A control numbered like one before it on the same card (Macro 2 after Macro 1) joins that
    // row; on a numbered sibling card (Osc 2 after Osc 1) it is left out, and a card left with
    // nothing new is named beside the first one. Other cards keep their own rows, even when an
    // earlier card had the same text (a Flanger's Rate after a Chorus's).
    std::set<std::pair<const juce::Component*, juce::String>> components; // a part the dock rebinds says something new
    std::map<juce::String, std::pair<std::size_t, std::size_t>> seen; // key -> section, entry
    for (const auto& f : found) {
        if (sections.empty() || sections.back().title != f.card)
            sections.push_back({f.card, {}, {}});
        // With the card in the key: a page that rebuilds its parts can give a new part the address
        // of one read before (the FX page's strips, lane by lane).
        if (!components.insert({f.component, f.card + "\x1f" + f.entry.titles[0] + "\x1f" + f.entry.text}).second)
            continue;
        const juce::String key = numberless(f.entry.titles[0]) + "\x1f" + f.entry.text;
        if (const auto it = seen.find(key); it != seen.end()) {
            const std::size_t where = it->second.first;
            if (where == sections.size() - 1) {
                sections.back().entries[it->second.second].titles.add(f.entry.titles[0]);
                continue;
            }
            if (numberless(sections[where].title) == numberless(sections.back().title))
                continue;
        }
        // The same text under another name on this card shares its row (Compare A and B).
        auto& entries = sections.back().entries;
        auto same = std::find_if(entries.begin(), entries.end(), [&](const HelpEntry& e) { return e.text == f.entry.text; });
        if (same != entries.end()) {
            same->titles.add(f.entry.titles[0]);
            seen[key] = {sections.size() - 1, static_cast<std::size_t>(same - entries.begin())};
            continue;
        }
        seen[key] = {sections.size() - 1, entries.size()};
        entries.push_back(f.entry);
    }
    // A card whose texts all appeared on an earlier card (Osc 2 after Osc 1) is named there instead.
    std::vector<HelpSection> merged;
    for (auto& s : sections) {
        if (s.entries.empty() && !merged.empty() && s.title.isNotEmpty() && s.title != merged.back().title) {
            if (!merged.back().sameAs.contains(s.title))
                merged.back().sameAs.add(s.title);
            continue;
        }
        if (!s.entries.empty())
            merged.push_back(std::move(s));
    }
    return merged;
}

void writeSections(juce::String& out, const std::vector<HelpSection>& sections, const juce::String& level) {
    for (const auto& s : sections) {
        juce::String heading = s.title.isEmpty() ? juce::String("On the page") : s.title;
        if (!s.sameAs.isEmpty())
            heading << " (also " << s.sameAs.joinIntoString(", ") << ")";
        out << level << " " << heading << "\n\n";
        out << "| Control | Help strip |\n|---|---|\n";
        for (const auto& e : s.entries) {
            juce::StringArray titles = e.titles;
            titles.removeEmptyStrings();
            titles.removeDuplicates(false);
            out << "| " << (titles.isEmpty() ? juce::String("-") : joinTitles(titles)) << " | " << e.text << " |\n";
        }
        out << "\n";
    }
}

const char* const kHelpBegin = "<!-- ui-help: begin (generated by zederia-cli ui-help from the interface's help-strip texts; do not edit by hand) -->";
const char* const kHelpEnd = "<!-- ui-help: end -->";

} // namespace

int cmdUiHelp(const Args& args) {
    juce::ScopedJuceInitialiser_GUI gui;
    // The reference describes the interface itself, whatever this computer has installed: the
    // built-in init sound (an empty factory folder, so no Default preset), an empty library and
    // default preferences with every control showing (not the Simple view).
    const juce::File scratch = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("zederia-ui-help");
    const juce::File emptyFactory = scratch.getChildFile("factory"), emptyUser = scratch.getChildFile("user");
    scratch.deleteRecursively();
    emptyFactory.createDirectory();
    emptyUser.createDirectory();
#ifdef _WIN32
    _putenv_s("ZEDERIA_FACTORY", emptyFactory.getFullPathName().toRawUTF8());
#else
    setenv("ZEDERIA_FACTORY", emptyFactory.getFullPathName().toRawUTF8(), 1);
#endif
    Prefs::useFile(scratch.getChildFile("settings-none.json"));
    Prefs::get().userFolder = emptyUser.getFullPathName();
    ZederiaProcessor processor;
    processor.setPlayConfigDetails(0, 2, 48000.0, 512);
    processor.prepareToPlay(48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> base(processor.createEditor());
    auto* editor = dynamic_cast<ZederiaEditor*>(base.get());
    if (editor == nullptr)
        return kFailed;
    editor->context().prefs.simpleView = false;
    editor->setScalePercent(100);

    const juce::StringArray names = ZederiaEditor::pageNames();
    juce::String out;
    out << kHelpBegin << "\n\n";
    std::vector<juce::Component*> pageRoots;
    for (int p = 0; p < names.size(); ++p)
        pageRoots.push_back(&editor->page(p));
    // The frame around the pages (header, dock, keyboard): everything that is not a page, with
    // each kind of modulator shown in the dock in turn.
    editor->showPage(0);
    editor->tickFrame();
    {
        std::vector<FoundHelp> found;
        int pass = 0;
        for (const char* source : {"env1", "env2", "lfo1", "flux1"}) {
            editor->dock().select(source);
            gatherWithTabs(*editor, *editor, pageRoots, pass, found);
        }
        editor->dock().select("env1");
        out << "### Around every page\n\n";
        writeSections(out, sectionsOf(std::move(found)), "####");
    }
    for (int p = 0; p < names.size(); ++p) {
        editor->showPage(p);
        std::vector<FoundHelp> found;
        int pass = 0;
        if (auto* fx = dynamic_cast<zui::FxPage*>(&editor->page(p))) {
            // Every effect, so every effect's strip is read: lane A holds the first eight, lane B the rest.
            const auto effects = zed::Registry::get().effectTypes();
            const int types = static_cast<int>(effects.size());
            for (int type = 0; type < types; ++type)
                fx->insertEffect(type < 8 ? 0 : 1, type < 8 ? type : type - 8, type);
            effectCardOf = [fx, effects](const zui::Widget& w, juce::String& card, int& order, juce::String& prefix) {
                // Slot controls are titled by their slot: "FX A1 Amount", "FX M2 Mix".
                const juce::String& title = w.helpTitle;
                const juce::String lanes = "ABM";
                if (!title.startsWith("FX ") || title.length() < 7 || lanes.indexOfChar(title[3]) < 0 || !juce::CharacterFunctions::isDigit(title[4]))
                    return false;
                const int lane = lanes.indexOfChar(title[3]), slot = title.substring(4).getIntValue() - 1;
                const int type = fx->slotType(lane, slot);
                if (type < 0 || type >= static_cast<int>(effects.size()))
                    return false;
                card = juce::String::fromUTF8(effects[static_cast<std::size_t>(type)].name.c_str());
                order = type;
                prefix = title.upToFirstOccurrenceOf(" ", true, false) + title.substring(3).upToFirstOccurrenceOf(" ", true, false);
                return true;
            };
            for (int lane = 0; lane < 3; ++lane) {
                fx->select(lane, 0);
                gatherWithTabs(*editor, editor->page(p), {}, pass, found);
            }
            effectCardOf = nullptr;
            fx->select(0, 0);
        } else {
            gatherWithTabs(*editor, editor->page(p), {}, pass, found);
        }
        out << "### The " << names[p] << " page\n\n";
        writeSections(out, sectionsOf(std::move(found)), "####");
    }
    out << kHelpEnd << "\n";
    base.reset();

    const std::string update = args.get("update", ""), check = args.get("check", "");
    const std::string target = update.empty() ? check : update;
    if (target.empty()) {
        std::printf("%s", out.toRawUTF8());
        return kOk;
    }
    // The generated block replaces the one between the markers in the manual.
    const juce::File file = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(target.c_str()));
    const juce::String manual = file.loadFileAsString().replace("\r\n", "\n");
    const int begin = manual.indexOf(kHelpBegin), end = manual.indexOf(kHelpEnd);
    if (begin < 0 || end < begin) {
        std::fprintf(stderr, "error: %s has no ui-help markers\n", target.c_str());
        return kFailed;
    }
    const juce::String updated = manual.substring(0, begin) + out + manual.substring(end + juce::String(kHelpEnd).length() + 1);
    if (!check.empty()) {
        if (updated != manual) {
            std::fprintf(stderr, "error: the interface reference in %s is out of date; run zederia-cli ui-help --update %s\n", target.c_str(), target.c_str());
            return kFailed;
        }
        std::printf("%s is up to date\n", target.c_str());
        return kOk;
    }
    if (!file.replaceWithText(updated, false, false, "\n")) {
        std::fprintf(stderr, "error: could not write %s\n", target.c_str());
        return kFailed;
    }
    std::printf("updated %s\n", target.c_str());
    return kOk;
}

} // namespace cli

#else

namespace cli {
int cmdUiSnapshot(const Args&) {
    std::fprintf(stderr, "error: this zederia-cli was built without the plug-in, so it cannot draw the interface\n");
    return kFailed;
}
int cmdUiHelp(const Args&) {
    std::fprintf(stderr, "error: this zederia-cli was built without the plug-in, so it cannot read the interface\n");
    return kFailed;
}
} // namespace cli

#endif
