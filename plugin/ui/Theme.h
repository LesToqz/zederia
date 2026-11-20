// Theme tokens, fonts and text drawing (plan 3.13 "Look"). Themes are token files
// (plugin/themes/*.json, compiled in; more can be dropped into Documents/Zederia/Themes). Every
// colour the interface paints comes from the current theme; nothing is a bitmap.
//
// Text goes through drawText(), which also feeds the snapshot checks: while an audit records, each
// string that does not fit its box, or is smaller than 11 px at 100 %, is noted against the
// component that drew it.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <string>
#include <vector>

namespace zui {

// The module families (plan 3.13). Controls of every family share one ink: colour is kept for
// signal and state (a lit power lamp, the trace that sounds, a modulation route, meters and
// warnings), so OSC, Filter and FX read as parts of one instrument. The family hues remain for
// the displays that draw signal flow (the signal map, the workshop).
enum class Family { Neutral, Sources, Filters, Modulators, Effects, Play };

struct Theme {
    juce::String name = "Graphite";
    bool dark = true;
    juce::Colour bg, panel, raised, sunken, line, lineStrong;
    juce::Colour text, textDim, textFaint;
    juce::Colour focus, selection, warning, danger, ok;
    juce::Colour display, displayGrid, displayLine;
    // The ink of an engaged control (value arcs, fader thumbs, a chosen segment, a switch that is
    // on) and the lamp of a module that is on. Optional in a token file (defaults: text, sources).
    juce::Colour control, lamp;
    juce::Colour sources, filters, modulators, effects, play, global;
    juce::Colour mod[8]; // modulation route colours, chosen to stay apart under colour-blindness
    float radius = 6.0f;
    float glow = 0.3f; // strength of the soft line under luminous display curves (0: none)
    // Grain strength on the walnut dock and the depth of machined controls (0: flat).
    float texture = 0.0f;
    float metal = 0.0f;

    // The control ink for a family (the same for every family; see above).
    juce::Colour accent(Family f) const;
    juce::Colour modColour(int index) const { return mod[((index % 8) + 8) % 8]; }

    // The theme in use. Changing it repaints nothing by itself: the editor repaints.
    static const Theme& current();
    static void setCurrent(const juce::String& name);
    static juce::StringArray names();
    // Parses a token file; false with a message when a colour is missing or malformed.
    static bool fromJson(const std::string& json, Theme& out, juce::String& error);
};

inline const Theme& theme() { return Theme::current(); }

// WCAG contrast ratio of two colours (1 .. 21).
double contrastRatio(juce::Colour a, juce::Colour b);

// --- Text ------------------------------------------------------------------------------------------
inline constexpr float kTextSmall = 11.0f;  // the smallest text anywhere at 100 % (plan 3.13): legends
inline constexpr float kValueText = 12.0f;  // a control's value: a step above its legend
inline constexpr float kPrimaryValueText = 14.5f; // the value of a primary control (cutoff, master)
inline constexpr float kTextBody = 12.5f;
inline constexpr float kTextTitle = 14.0f;
inline constexpr float kTextLarge = 17.0f;

// The text size preference: every size below is multiplied by it when text is drawn or measured
// (0.9 to 1.5; 1 is the size the layout was made for). drawText() keeps enlarged text inside its
// box by giving back as much of the enlargement as it must, never going below the base size.
void setTextScale(float scale);
float textScale();

juce::Font font(float size, bool bold = false);
float textWidth(const juce::String& text, float size, bool bold = false);
// Draws one line in a box. Text that does not fit is drawn with an ellipsis (and recorded by an audit).
void drawText(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> box, float size, juce::Colour colour,
              juce::Justification just = juce::Justification::centredLeft, bool bold = false);
// The largest size, from `size` down to kTextSmall, at which the text fits the width.
float fittingSize(const juce::String& text, float width, float size, bool bold = false);

// --- Audit records (the snapshot checks, plan 6.1) ---------------------------------------------------
struct TextRecord {
    const juce::Component* component = nullptr;
    juce::String text;
    float width = 0, needed = 0, size = 0;
};
class AuditRecorder {
public:
    static void start();
    static void stop();
    static bool active();
    static void note(const TextRecord& record);
    static std::vector<TextRecord> take();
    // The component whose paint() is running (set by zui::Widget).
    static const juce::Component*& painting();
};

// A look and feel for the framework's own parts (menus, text fields, scroll bars, tooltips) in the
// theme's colours and font.
class LookAndFeel final : public juce::LookAndFeel_V4 {
public:
    LookAndFeel();
    void applyTheme();
    juce::Font getPopupMenuFont() override;
    juce::Font getTextButtonFont(juce::TextButton&, int) override;
    juce::Font getLabelFont(juce::Label&) override;
    juce::Typeface::Ptr getTypefaceForFont(const juce::Font&) override;
    void drawPopupMenuBackground(juce::Graphics&, int width, int height) override;
    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool vertical, int thumbStart,
                       int thumbSize, bool mouseOver, bool mouseDown) override;
    int getDefaultScrollbarWidth() override { return 10; }
    void fillTextEditorBackground(juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline(juce::Graphics&, int width, int height, juce::TextEditor&) override;
    juce::Rectangle<int> getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;
    void drawTooltip(juce::Graphics&, const juce::String& text, int width, int height) override;
    void drawTableHeaderBackground(juce::Graphics&, juce::TableHeaderComponent&) override;
    void drawTableHeaderColumn(juce::Graphics&, juce::TableHeaderComponent&, const juce::String& columnName, int columnId, int width, int height,
                               bool isMouseOver, bool isMouseDown, int columnFlags) override;
    static juce::Path glyphPathForHeader(bool up, juce::Rectangle<float> box);
};

} // namespace zui
