#include "Audit.h"

#include "Widgets.h"

namespace zui {

namespace {

const juce::Identifier kOverlay("zuiOverlay");
const juce::Identifier kScrolling("zuiScrolling");

bool isOverlay(const juce::Component& c) {
    // The window's resize corner sits over the content by design.
    return c.getProperties().getWithDefault(kOverlay, false) || dynamic_cast<const juce::ResizableCornerComponent*>(&c) != nullptr ||
           dynamic_cast<const juce::ResizableBorderComponent*>(&c) != nullptr;
}
bool isScrolling(const juce::Component& c) { return c.getProperties().getWithDefault(kScrolling, false); }

// Framework parts whose insides are not ours to check.
bool opaqueToAudit(const juce::Component& c) {
    return dynamic_cast<const juce::TextEditor*>(&c) != nullptr || dynamic_cast<const juce::ScrollBar*>(&c) != nullptr ||
           dynamic_cast<const juce::ListBox*>(&c) != nullptr || dynamic_cast<const juce::MidiKeyboardComponent*>(&c) != nullptr ||
           dynamic_cast<const juce::ComboBox*>(&c) != nullptr;
}

void visit(juce::Component& c, std::vector<Finding>& out) {
    if (!c.isVisible())
        return;
    const auto* widget = dynamic_cast<const Widget*>(&c);
    if (c.getWantsKeyboardFocus() && c.isEnabled()) {
        if (c.getTitle().trim().isEmpty())
            out.push_back({"unlabeled", componentPath(c), "an interactive control without an accessible name"});
        else if (widget != nullptr && !widget->hasVisibleLabel() && c.getTitle().isEmpty())
            out.push_back({"unlabeled", componentPath(c), "no visible label"});
        if (c.getWidth() < 4 || c.getHeight() < 4)
            out.push_back({"zero_size", componentPath(c), "a control with no room: " + c.getBounds().toString()});
        else if (c.getHeight() < 16 || c.getWidth() < 16)
            out.push_back({"cramped", componentPath(c), "a control too small to use: " + c.getBounds().toString()});
    }
    if (opaqueToAudit(c))
        return;
    if (auto* viewport = dynamic_cast<juce::Viewport*>(&c)) {
        if (auto* content = viewport->getViewedComponent())
            visit(*content, out);
        return;
    }
    const auto local = c.getLocalBounds().toFloat().expanded(0.6f);
    std::vector<juce::Component*> kids;
    for (auto* child : c.getChildren())
        if (child->isVisible() && child->getWidth() > 0 && child->getHeight() > 0)
            kids.push_back(child);
    for (auto* child : kids) {
        const auto box = child->getBoundsInParent().toFloat();
        if (!isScrolling(c) && !isOverlay(*child) && !local.contains(box))
            out.push_back({"clipped", componentPath(*child), "reaches outside its parent: " + box.toString() + " in " + c.getLocalBounds().toString()});
    }
    for (std::size_t i = 0; i < kids.size(); ++i) {
        if (isOverlay(*kids[i]))
            continue;
        for (std::size_t j = i + 1; j < kids.size(); ++j) {
            if (isOverlay(*kids[j]))
                continue;
            const auto overlap = kids[i]->getBoundsInParent().toFloat().getIntersection(kids[j]->getBoundsInParent().toFloat());
            if (overlap.getWidth() > 1.0f && overlap.getHeight() > 1.0f)
                out.push_back({"overlap", componentPath(*kids[i]), "overlaps " + componentPath(*kids[j]) + " by " + overlap.toString()});
        }
    }
    for (auto* child : kids)
        visit(*child, out);
}

} // namespace

void markOverlay(juce::Component& c) { c.getProperties().set(kOverlay, true); }
void markScrolling(juce::Component& c) { c.getProperties().set(kScrolling, true); }

juce::String componentPath(const juce::Component& c) {
    juce::StringArray parts;
    for (const juce::Component* p = &c; p != nullptr; p = p->getParentComponent()) {
        juce::String name = p->getTitle();
        if (name.isEmpty())
            name = p->getName();
        if (name.isEmpty())
            name = p->getComponentID();
        if (name.isEmpty())
            continue;
        parts.insert(0, name);
    }
    return parts.joinIntoString("/");
}

juce::Image renderSnapshot(juce::Component& root, std::vector<Finding>* findings) {
    juce::Image image(juce::Image::ARGB, juce::jmax(1, root.getWidth()), juce::jmax(1, root.getHeight()), true, juce::SoftwareImageType());
    {
        juce::Graphics g(image);
        g.fillAll(theme().bg);
        AuditRecorder::start();
        root.paintEntireComponent(g, true);
        AuditRecorder::stop();
    }
    const auto records = AuditRecorder::take();
    if (findings != nullptr) {
        for (const auto& r : records) {
            const juce::String where = r.component != nullptr ? componentPath(*r.component) : juce::String("?");
            if (r.size < kTextSmall - 0.01f)
                findings->push_back({"small_text", where, "\"" + r.text + "\" at " + juce::String(r.size, 1) + " px"});
            else if (r.needed > r.width + 0.75f)
                findings->push_back({"truncated", where, "\"" + r.text + "\" needs " + juce::String(r.needed, 1) + " px, has " + juce::String(r.width, 1)});
            else
                findings->push_back({"truncated", where, "\"" + r.text + "\" is taller than its box"});
        }
    }
    return image;
}

bool writePng(const juce::Image& image, const juce::File& file) {
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::FileOutputStream out(file);
    if (!out.openedOk())
        return false;
    juce::PNGImageFormat png;
    return png.writeImageToStream(image, out);
}

std::vector<Finding> checkLayout(juce::Component& root) {
    std::vector<Finding> out;
    // The root may be an editor that is not on screen (offscreen snapshots): look inside it anyway.
    const bool wasVisible = root.isVisible();
    root.setVisible(true);
    visit(root, out);
    root.setVisible(wasVisible);
    return out;
}

std::vector<Finding> checkTheme(const Theme& t) {
    std::vector<Finding> out;
    const double textMin = t.name == "Contrast" ? 7.0 : 4.5;
    const auto check = [&](const char* fg, juce::Colour a, const char* bg, juce::Colour b, double min) {
        const double ratio = contrastRatio(a, b);
        if (ratio < min)
            out.push_back({"contrast", t.name + ": " + fg + " on " + bg, juce::String(ratio, 2) + ":1, needs " + juce::String(min, 1) + ":1"});
    };
    for (const auto& [name, surface] : {std::pair{"bg", t.bg}, std::pair{"panel", t.panel}, std::pair{"raised", t.raised}, std::pair{"sunken", t.sunken}}) {
        check("text", t.text, name, surface, textMin);
        check("textDim", t.textDim, name, surface, textMin);
        check("textFaint", t.textFaint, name, surface, std::min(textMin, 4.5));
    }
    check("text", t.text, "selection", t.selection, textMin);
    check("textDim", t.textDim, "display", t.display, textMin);
    check("bg (text on accent buttons)", t.bg, "sources", t.sources, 4.5);
    for (const auto& [name, accent] : {std::pair{"sources", t.sources}, std::pair{"filters", t.filters}, std::pair{"modulators", t.modulators},
                                       std::pair{"effects", t.effects}, std::pair{"play", t.play}, std::pair{"focus", t.focus}}) {
        check(name, accent, "panel", t.panel, 3.0);
        check(name, accent, "display", t.display, 3.0);
        check("bg (text on accent)", t.bg, name, accent, 4.5);
    }
    for (int i = 0; i < 8; ++i) {
        check(("mod" + std::to_string(i + 1)).c_str(), t.mod[i], "panel", t.panel, 3.0);
        check(("mod" + std::to_string(i + 1)).c_str(), t.mod[i], "display", t.display, 3.0);
    }
    check("line strong", t.lineStrong, "panel", t.panel, 1.6);
    return out;
}

} // namespace zui
