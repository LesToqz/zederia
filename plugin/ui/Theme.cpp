#include "Theme.h"

#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace zui {

namespace themes {
extern const std::string_view graphite; // generated from plugin/themes/graphite.json
}

namespace {

struct Registry {
    std::map<juce::String, Theme> themes;
    juce::StringArray order;
    juce::String current = "Graphite";
    Registry() {
        for (const auto json : {themes::graphite}) {
            Theme t;
            juce::String error;
            if (Theme::fromJson(std::string(json), t, error)) {
                order.add(t.name);
                themes[t.name] = t;
            } else {
                jassertfalse; // a built-in token file is broken
            }
        }
    }
};

Registry& registry() {
    static Registry r;
    return r;
}

bool parseColour(const std::string& text, juce::Colour& out) {
    if (text.size() != 7 && text.size() != 9)
        return false;
    if (text[0] != '#')
        return false;
    for (std::size_t i = 1; i < text.size(); ++i)
        if (!std::isxdigit(static_cast<unsigned char>(text[i])))
            return false;
    const auto value = static_cast<juce::uint32>(std::stoul(text.substr(1), nullptr, 16));
    out = text.size() == 7 ? juce::Colour(0xff000000u | value) : juce::Colour(value);
    return true;
}

double luminance(juce::Colour c) {
    const auto channel = [](juce::uint8 v) {
        const double s = v / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(c.getRed()) + 0.7152 * channel(c.getGreen()) + 0.0722 * channel(c.getBlue());
}

struct AuditState {
    bool active = false;
    std::vector<TextRecord> records;
};
AuditState& audit() {
    static AuditState s;
    return s;
}

juce::String chosenFace() {
    static const juce::String face = [] {
        const auto names = juce::Font::findAllTypefaceNames();
        for (const char* candidate : {"Segoe UI", "Helvetica Neue", "Arial"})
            if (names.contains(candidate))
                return juce::String(candidate);
        return juce::Font::getDefaultSansSerifFontName();
    }();
    return face;
}

} // namespace

juce::Colour Theme::accent(Family f) const {
    switch (f) {
    case Family::Sources: return sources;
    case Family::Filters: return filters;
    case Family::Modulators: return modulators;
    case Family::Effects: return effects;
    case Family::Play: return play;
    case Family::Neutral: break;
    }
    return global;
}

const Theme& Theme::current() {
    auto& r = registry();
    const auto it = r.themes.find(r.current);
    return it != r.themes.end() ? it->second : r.themes.begin()->second;
}

void Theme::setCurrent(const juce::String& name) {
    auto& r = registry();
    if (r.themes.count(name) > 0)
        r.current = name;
}

juce::StringArray Theme::names() { return registry().order; }

bool Theme::fromJson(const std::string& json, Theme& out, juce::String& error) {
    try {
        const auto doc = nlohmann::json::parse(json);
        if (!doc.is_object() || doc.value("format", "") != "zederia.theme") {
            error = "not a Zederia theme";
            return false;
        }
        Theme t;
        t.name = juce::String::fromUTF8(doc.value("name", "Untitled").c_str());
        t.dark = doc.value("dark", true);
        const auto& colours = doc.at("colours");
        const auto read = [&](const char* key, juce::Colour& c) {
            const auto it = colours.find(key);
            if (it == colours.end() || !it->is_string() || !parseColour(it->get<std::string>(), c))
                throw std::runtime_error(std::string("colour \"") + key + "\" is missing or not #RRGGBB");
        };
        read("bg", t.bg); read("panel", t.panel); read("raised", t.raised); read("sunken", t.sunken);
        read("line", t.line); read("lineStrong", t.lineStrong);
        read("text", t.text); read("textDim", t.textDim); read("textFaint", t.textFaint);
        read("focus", t.focus); read("selection", t.selection); read("warning", t.warning); read("danger", t.danger); read("ok", t.ok);
        read("display", t.display); read("displayGrid", t.displayGrid); read("displayLine", t.displayLine);
        read("sources", t.sources); read("filters", t.filters); read("modulators", t.modulators); read("effects", t.effects);
        read("play", t.play); read("global", t.global);
        for (int i = 0; i < 8; ++i)
            read(("mod" + std::to_string(i + 1)).c_str(), t.mod[i]);
        if (doc.contains("metrics")) {
            t.radius = doc["metrics"].value("radius", 6.0f);
            t.glow = doc["metrics"].value("glow", 0.3f);
            t.texture = doc["metrics"].value("texture", 0.0f);
            t.metal = doc["metrics"].value("metal", 0.0f);
        }
        out = t;
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}


double contrastRatio(juce::Colour a, juce::Colour b) {
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// --- Text -------------------------------------------------------------------------------------------

namespace {
std::atomic<float> gTextScale{1.0f};
} // namespace

void setTextScale(float scale) { gTextScale = juce::jlimit(0.9f, 1.5f, scale); }
float textScale() { return gTextScale.load(); }

juce::Font font(float size, bool bold) {
    return juce::Font(juce::FontOptions(chosenFace(), size * textScale(), bold ? juce::Font::bold : juce::Font::plain));
}

float textWidth(const juce::String& text, float size, bool bold) {
    // Measured once per text and size: controls fit their labels on every repaint, and measuring
    // shapes the text each time.
    struct Widths {
        std::mutex mutex;
        std::unordered_map<juce::String, float> values;
    };
    static Widths cache;
    const juce::String key = text + "\x1f" + juce::String(size * textScale(), 2) + (bold ? "b" : "");
    {
        const std::lock_guard<std::mutex> lock(cache.mutex);
        if (const auto it = cache.values.find(key); it != cache.values.end())
            return it->second;
    }
    const float width = juce::GlyphArrangement::getStringWidth(font(size, bold), text);
    const std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.values.size() > 4096)
        cache.values.clear();
    cache.values.emplace(key, width);
    return width;
}

float fittingSize(const juce::String& text, float width, float size, bool bold) {
    while (size > kTextSmall && textWidth(text, size, bold) > width)
        size -= 0.5f;
    return std::max(size, kTextSmall);
}

void drawText(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> box, float size, juce::Colour colour,
              juce::Justification just, bool bold) {
    if (text.isEmpty())
        return;
    const bool auditing = AuditRecorder::active();
    const bool visible = g.clipRegionIntersects(box.getSmallestIntegerContainer().expanded(2));
    if (!visible && !auditing)
        return; // outside the area being repainted: laying the glyphs out would be wasted
    const float scale = textScale();
    if (scale > 1.0f) {
        // Larger text where there is room; where there is not, as large as fits, down to the size
        // the layout was made for (which the snapshot checks have seen fit).
        const float base = size / scale;
        const float tallest = (box.getHeight() + 2.0f) / scale;
        if (size > tallest)
            size = std::max(base, tallest);
        while (size > base && textWidth(text, size, bold) > box.getWidth())
            size = std::max(base, size - 0.25f);
    }
    if (auditing) { // what is drawn: the width at the size used, the height in pixels
        const float needed = textWidth(text, size, bold);
        if (needed > box.getWidth() + 0.75f || size < kTextSmall / scale - 0.01f || size * scale > box.getHeight() + 2.0f)
            AuditRecorder::note({AuditRecorder::painting(), text, box.getWidth(), needed, size * scale});
    }
    if (!visible)
        return;
    g.setColour(colour);
    g.setFont(font(size, bold));
    g.drawText(text, box, just, true);
}

void AuditRecorder::start() {
    audit().active = true;
    audit().records.clear();
}
void AuditRecorder::stop() { audit().active = false; }
bool AuditRecorder::active() { return audit().active; }
void AuditRecorder::note(const TextRecord& record) { audit().records.push_back(record); }
std::vector<TextRecord> AuditRecorder::take() { return std::exchange(audit().records, {}); }
const juce::Component*& AuditRecorder::painting() {
    static const juce::Component* component = nullptr;
    return component;
}

// --- Look and feel ------------------------------------------------------------------------------------

LookAndFeel::LookAndFeel() { applyTheme(); }

void LookAndFeel::applyTheme() {
    const Theme& t = theme();
    setColour(juce::PopupMenu::backgroundColourId, t.raised);
    setColour(juce::PopupMenu::textColourId, t.text);
    setColour(juce::PopupMenu::headerTextColourId, t.textDim);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, t.selection);
    setColour(juce::PopupMenu::highlightedTextColourId, t.text);
    setColour(juce::TextEditor::backgroundColourId, t.sunken);
    setColour(juce::TextEditor::textColourId, t.text);
    setColour(juce::TextEditor::highlightColourId, t.selection);
    setColour(juce::TextEditor::highlightedTextColourId, t.text);
    setColour(juce::TextEditor::outlineColourId, t.line);
    setColour(juce::TextEditor::focusedOutlineColourId, t.focus);
    setColour(juce::CaretComponent::caretColourId, t.focus);
    setColour(juce::ScrollBar::thumbColourId, t.lineStrong);
    setColour(juce::ScrollBar::trackColourId, t.sunken);
    setColour(juce::TooltipWindow::backgroundColourId, t.raised);
    setColour(juce::TooltipWindow::textColourId, t.text);
    setColour(juce::TooltipWindow::outlineColourId, t.lineStrong);
    setColour(juce::Label::textColourId, t.text);
    setColour(juce::ListBox::backgroundColourId, t.panel);
    setColour(juce::ResizableWindow::backgroundColourId, t.bg);
    setColour(juce::DocumentWindow::textColourId, t.text);
    setColour(juce::TextButton::buttonColourId, t.raised);
    setColour(juce::TextButton::textColourOffId, t.text);
    setColour(juce::ComboBox::backgroundColourId, t.raised);
    setColour(juce::ComboBox::textColourId, t.text);
    setColour(juce::ComboBox::outlineColourId, t.line);
    setColour(juce::AlertWindow::backgroundColourId, t.raised);
    setColour(juce::AlertWindow::textColourId, t.text);
    setColour(juce::AlertWindow::outlineColourId, t.lineStrong);
    setColour(juce::DirectoryContentsDisplayComponent::highlightColourId, t.selection);
    setColour(juce::DirectoryContentsDisplayComponent::textColourId, t.text);
    setColour(juce::FileSearchPathListComponent::backgroundColourId, t.panel);
}

juce::Font LookAndFeel::getPopupMenuFont() { return font(13.0f); }
juce::Font LookAndFeel::getTextButtonFont(juce::TextButton&, int) { return font(kTextBody); }
juce::Font LookAndFeel::getLabelFont(juce::Label&) { return font(kTextBody); }

juce::Typeface::Ptr LookAndFeel::getTypefaceForFont(const juce::Font& f) {
    if (f.getTypefaceName() == juce::Font::getDefaultSansSerifFontName()) {
        juce::Font replaced(f);
        replaced.setTypefaceName(chosenFace());
        return juce::LookAndFeel_V4::getTypefaceForFont(replaced);
    }
    return juce::LookAndFeel_V4::getTypefaceForFont(f);
}

void LookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height) {
    const Theme& t = theme();
    g.fillAll(t.raised);
    g.setColour(t.lineStrong);
    g.drawRect(0, 0, width, height, 1);
}

void LookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height, bool vertical,
                                int thumbStart, int thumbSize, bool mouseOver, bool mouseDown) {
    const Theme& t = theme();
    juce::Rectangle<float> thumb = vertical ? juce::Rectangle<float>(static_cast<float>(x) + 2.0f, static_cast<float>(thumbStart), static_cast<float>(width) - 4.0f, static_cast<float>(thumbSize))
                                            : juce::Rectangle<float>(static_cast<float>(thumbStart), static_cast<float>(y) + 2.0f, static_cast<float>(thumbSize), static_cast<float>(height) - 4.0f);
    g.setColour(mouseDown ? t.textDim : mouseOver ? t.lineStrong.brighter(0.2f) : t.lineStrong);
    g.fillRoundedRectangle(thumb, 3.0f);
}

void LookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor&) {
    g.setColour(theme().sunken);
    g.fillRoundedRectangle(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 4.0f);
}

void LookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& editor) {
    const Theme& t = theme();
    g.setColour(editor.hasKeyboardFocus(true) ? t.lineStrong : t.line); // the caret shows where typing goes; nothing lights up
    g.drawRoundedRectangle(0.5f, 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f, 4.0f, 1.0f);
}

juce::Rectangle<int> LookAndFeel::getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) {
    const int w = juce::jmin(360, static_cast<int>(textWidth(tipText, kTextBody)) + 20);
    const int lines = static_cast<int>(std::ceil(textWidth(tipText, kTextBody) / 340.0f));
    const int h = 10 + 17 * juce::jmax(1, lines);
    return juce::Rectangle<int>(screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6, w, h)
        .constrainedWithin(parentArea);
}

void LookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height) {
    const Theme& t = theme();
    g.setColour(t.raised);
    g.fillRoundedRectangle(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 4.0f);
    g.setColour(t.lineStrong);
    g.drawRoundedRectangle(0.5f, 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f, 4.0f, 1.0f);
    g.setColour(t.text);
    g.setFont(font(kTextBody));
    g.drawFittedText(text, juce::Rectangle<int>(8, 4, width - 16, height - 8), juce::Justification::centredLeft, 6, 1.0f);
}

} // namespace zui

namespace zui {

void LookAndFeel::drawTableHeaderBackground(juce::Graphics& g, juce::TableHeaderComponent& header) {
    const Theme& t = theme();
    g.fillAll(t.raised);
    g.setColour(t.line);
    g.fillRect(header.getLocalBounds().removeFromBottom(1));
}

void LookAndFeel::drawTableHeaderColumn(juce::Graphics& g, juce::TableHeaderComponent&, const juce::String& columnName, int, int width, int height,
                                        bool isMouseOver, bool, int columnFlags) {
    const Theme& t = theme();
    auto area = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(6.0f, 0.0f);
    if (isMouseOver) {
        g.setColour(t.raised.brighter(0.08f));
        g.fillRect(0, 0, width, height);
    }
    const bool sorted = (columnFlags & (juce::TableHeaderComponent::sortedForwards | juce::TableHeaderComponent::sortedBackwards)) != 0;
    if (sorted) {
        const auto arrow = area.removeFromRight(10.0f).withSizeKeepingCentre(8.0f, 8.0f);
        g.setColour(t.focus);
        g.fillPath(glyphPathForHeader((columnFlags & juce::TableHeaderComponent::sortedForwards) != 0, arrow));
    }
    drawText(g, columnName, area, kTextSmall + 0.5f, sorted ? t.text : t.textDim, juce::Justification::centredLeft, true);
    g.setColour(t.line);
    g.fillRect(width - 1, 4, 1, height - 8);
}

juce::Path LookAndFeel::glyphPathForHeader(bool up, juce::Rectangle<float> b) {
    juce::Path p;
    if (up)
        p.addTriangle(b.getCentreX(), b.getY(), b.getRight(), b.getBottom(), b.getX(), b.getBottom());
    else
        p.addTriangle(b.getX(), b.getY(), b.getRight(), b.getY(), b.getCentreX(), b.getBottom());
    return p;
}

} // namespace zui
