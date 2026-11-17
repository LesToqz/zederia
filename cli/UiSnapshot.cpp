// zederia-cli ui-snapshot: editor pages rendered offscreen to PNG at chosen scales and themes,
// with the automatic interface checks of plan 6.1. Built when the plug-in is part of the build.
#include "CliUtil.h"

#if ZED_HAVE_UI
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ui/Audit.h"

#include <juce_events/juce_events.h>

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

} // namespace cli

#else

namespace cli {
int cmdUiSnapshot(const Args&) {
    std::fprintf(stderr, "error: this zederia-cli was built without the plug-in, so it cannot draw the interface\n");
    return kFailed;
}
} // namespace cli

#endif
