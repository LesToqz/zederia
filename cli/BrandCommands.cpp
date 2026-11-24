// zederia-cli brand: the icons and installer images, made from the logo's SVG file
// (plugin/brand/zederia-mark.svg) so they can be rebuilt whenever it changes. Built when the
// plug-in is part of the build (it draws with the framework's SVG renderer).
#include "CliUtil.h"

#if ZED_HAVE_UI
#include "ui/Brand.h"

#include <juce_events/juce_events.h>

namespace cli {
namespace {

bool writePng(const juce::Image& image, const juce::File& file) {
    file.deleteFile();
    juce::FileOutputStream stream(file);
    juce::PNGImageFormat png;
    if (stream.openedOk() && png.writeImageToStream(image, stream))
        return true;
    std::fprintf(stderr, "error: cannot write %s\n", file.getFullPathName().toRawUTF8());
    return false;
}

} // namespace

int cmdBrand(const Args& args) {
    if (!args.has("svg") || !args.has("out")) {
        std::fprintf(stderr, "error: usage: zederia-cli brand --svg plugin/brand/zederia-mark.svg --out plugin/brand [--installer installer/images]\n");
        return kUsage;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    juce::String error;
    const zui::brand::Logo logo = zui::brand::loadLogo(juce::File::getCurrentWorkingDirectory().getChildFile(args.get("svg")), error);
    if (logo.drawable == nullptr) {
        std::fprintf(stderr, "error: %s\n", error.toRawUTF8());
        return kFailed;
    }
    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile(args.get("out"));
    const juce::File pngs = out.getChildFile("png");
    if (!pngs.createDirectory()) {
        std::fprintf(stderr, "error: cannot make %s\n", pngs.getFullPathName().toRawUTF8());
        return kFailed;
    }
    bool ok = true;
    std::vector<juce::Image> images;
    for (const int size : {16, 20, 24, 32, 40, 48, 64, 96, 128, 256}) {
        const juce::Image icon = zui::brand::renderIcon(logo, size);
        images.push_back(icon);
        ok = writePng(icon, pngs.getChildFile("zederia-" + juce::String(size) + ".png")) && ok;
        const zui::brand::GapReading gap = zui::brand::measureGap(icon);
        const bool checked = size == 16 || size == 32;
        std::printf("%4d px: gap between the slabs %3.0f %% covered over %d row(s), slabs %3.0f %% covered%s\n", size, gap.gap * 100.0f,
                    gap.gapRows, gap.slabs * 100.0f, checked ? (gap.visible() ? "  (checked: visible)" : "  (checked: CLOSED)") : "");
        if (checked && !gap.visible())
            ok = false;
    }
    const juce::MemoryBlock ico = zui::brand::iconFile(images);
    if (!out.getChildFile("zederia.ico").replaceWithData(ico.getData(), ico.getSize())) {
        std::fprintf(stderr, "error: cannot write zederia.ico\n");
        ok = false;
    }
    std::printf("wrote %s (%zu sizes, %zu bytes) and the PNGs in %s\n", out.getChildFile("zederia.ico").getFullPathName().toRawUTF8(),
                images.size(), ico.getSize(), pngs.getFullPathName().toRawUTF8());
    if (args.has("installer")) {
        // The sizes the installer's wizard asks for at 100 to 250 % display scaling.
        const juce::File dir = juce::File::getCurrentWorkingDirectory().getChildFile(args.get("installer"));
        dir.createDirectory();
        const int side[][2] = {{202, 386}, {269, 515}, {336, 643}, {403, 772}, {430, 824}, {498, 953}, {534, 1022}};
        const int small[] = {58, 77, 97, 116, 124, 143, 159};
        for (const auto& s : side)
            ok = writePng(zui::brand::installerSideImage(logo, s[0], s[1]),
                          dir.getChildFile("wizard-side-" + juce::String(s[0]) + "x" + juce::String(s[1]) + ".png")) && ok;
        for (const int s : small)
            ok = writePng(zui::brand::installerSmallImage(logo, s), dir.getChildFile("wizard-small-" + juce::String(s) + ".png")) && ok;
        std::printf("wrote the installer images in %s\n", dir.getFullPathName().toRawUTF8());
    }
    return ok ? kOk : kFailed;
}

} // namespace cli

#else

namespace cli {
int cmdBrand(const Args&) {
    std::fprintf(stderr, "error: this zederia-cli was built without the plug-in, so it cannot draw the logo\n");
    return kFailed;
}
} // namespace cli

#endif
