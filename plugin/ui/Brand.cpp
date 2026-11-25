#include "Brand.h"

#include <algorithm>
#include <cmath>

namespace zui::brand {

juce::Colour markColour() { return juce::Colour(0xffefb96bu); }

juce::Rectangle<float> markViewBox() { return {9.8f, 10.3f, 110.4f, 110.4f}; }

juce::Path mark() {
    // The same points as plugin/brand/zederia-mark.svg (checked by a test).
    juce::Path p;
    p.startNewSubPath(17, 26); p.lineTo(110, 26); p.lineTo(110, 43); p.lineTo(49, 86);
    p.lineTo(24, 86); p.lineTo(24, 80); p.lineTo(82, 39); p.lineTo(17, 39); p.closeSubPath();
    p.startNewSubPath(18, 94); p.lineTo(81, 94); p.lineTo(96, 82); p.lineTo(113, 82);
    p.lineTo(113, 91); p.lineTo(96, 105); p.lineTo(18, 105); p.closeSubPath();
    p.setUsingNonZeroWinding(false);
    return p;
}

juce::Path Wordmark::letters() const {
    juce::Path all(firstE);
    all.addPath(d);
    all.addPath(rest);
    return all;
}

Wordmark wordmark() {
    Wordmark w;
    const auto e = [](juce::Path& p, float x) {
        p.startNewSubPath(x + 11, 0); p.lineTo(x, 0); p.lineTo(x, 16); p.lineTo(x + 11, 16);
        p.startNewSubPath(x, 8); p.lineTo(x + 8, 8);
    };
    e(w.firstE, 28);
    w.d.startNewSubPath(45, 0); w.d.lineTo(53, 0); w.d.lineTo(58, 4); w.d.lineTo(58, 12); w.d.lineTo(53, 16); w.d.lineTo(45, 16);
    w.d.closeSubPath();
    e(w.rest, 64);
    w.rest.startNewSubPath(81, 16); w.rest.lineTo(81, 0); w.rest.lineTo(91, 0); w.rest.lineTo(94, 3);
    w.rest.lineTo(94, 6); w.rest.lineTo(91, 9); w.rest.lineTo(82, 9);
    w.rest.startNewSubPath(88, 9); w.rest.lineTo(95, 16);
    w.rest.startNewSubPath(101, 0); w.rest.lineTo(101, 16);
    w.rest.startNewSubPath(107, 16); w.rest.lineTo(113, 0); w.rest.lineTo(116, 0); w.rest.lineTo(122, 16);
    w.rest.startNewSubPath(110, 10); w.rest.lineTo(119, 10);
    return w;
}

juce::PathStrokeType wordmarkStroke() { return {2.6f, juce::PathStrokeType::mitered, juce::PathStrokeType::square}; }

juce::AffineTransform markBesideLetters(const Wordmark& word) {
    const auto stroke = wordmarkStroke();
    const auto inked = [&](const juce::Path& p) {
        juce::Path outline;
        stroke.createStrokedPath(outline, p);
        return outline.getBounds();
    };
    const auto capital = inked(word.firstE), shape = mark().getBounds();
    const float scale = capital.getHeight() / shape.getHeight();
    const float markRight = capital.getX() - (inked(word.d).getX() - capital.getRight());
    return juce::AffineTransform::translation(-shape.getRight(), -shape.getY()).scaled(scale).translated(markRight, capital.getY());
}

// --- Icons and installer images -------------------------------------------------------------------

Logo loadLogo(const juce::File& svg, juce::String& error) {
    Logo logo;
    const auto xml = juce::XmlDocument::parse(svg);
    if (xml == nullptr || !xml->hasTagName("svg")) {
        error = svg.getFullPathName() + " is not an SVG file";
        return logo;
    }
    const auto width = static_cast<float>(xml->getDoubleAttribute("width")), height = static_cast<float>(xml->getDoubleAttribute("height"));
    if (!(width > 0.0f) || width != height) {
        error = "the logo must be drawn in a square (equal width and height)";
        return logo;
    }
    logo.drawable = juce::Drawable::createFromSVGFile(svg);
    if (logo.drawable == nullptr) {
        error = "the SVG could not be drawn";
        return logo;
    }
    logo.viewport = width;
    // The drawing must be the mark, placed by the viewBox: compare the outline with the mark's
    // own points mapped the same way, so the icons can never drift from the logo's shape.
    const auto box = markViewBox();
    const float k = width / box.getWidth();
    const auto expected = mark().getBoundsTransformed(juce::AffineTransform::translation(-box.getX(), -box.getY()).scaled(k));
    const auto drawn = logo.drawable->getOutlineAsPath().getBounds();
    if (std::abs(drawn.getX() - expected.getX()) > 0.01f * width || std::abs(drawn.getRight() - expected.getRight()) > 0.01f * width ||
        std::abs(drawn.getY() - expected.getY()) > 0.01f * width || std::abs(drawn.getBottom() - expected.getBottom()) > 0.01f * width) {
        error = "the SVG's shape is not the mark the interface draws (plugin/ui/Brand.cpp): update both together";
        logo.drawable.reset();
    }
    return logo;
}

juce::Image renderIcon(const Logo& logo, int size) {
    constexpr int kOver = 8;
    juce::Image out(juce::Image::ARGB, size, size, true, juce::SoftwareImageType());
    if (logo.drawable == nullptr || size < 1)
        return out;
    juce::Image big(juce::Image::ARGB, size * kOver, size * kOver, true, juce::SoftwareImageType());
    {
        juce::Graphics g(big);
        logo.drawable->draw(g, 1.0f, juce::AffineTransform::scale(static_cast<float>(size * kOver) / logo.viewport));
    }
    const juce::Image::BitmapData from(big, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData to(out, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int sum[4] = {};
            for (int dy = 0; dy < kOver; ++dy) {
                for (int dx = 0; dx < kOver; ++dx) {
                    const auto* p = reinterpret_cast<const juce::PixelARGB*>(from.getPixelPointer(x * kOver + dx, y * kOver + dy));
                    sum[0] += p->getAlpha();
                    sum[1] += p->getRed();
                    sum[2] += p->getGreen();
                    sum[3] += p->getBlue();
                }
            }
            constexpr int n = kOver * kOver;
            auto* q = reinterpret_cast<juce::PixelARGB*>(to.getPixelPointer(x, y));
            q->setARGB(static_cast<juce::uint8>((sum[0] + n / 2) / n), static_cast<juce::uint8>((sum[1] + n / 2) / n),
                       static_cast<juce::uint8>((sum[2] + n / 2) / n), static_cast<juce::uint8>((sum[3] + n / 2) / n));
        }
    }
    return out;
}

GapReading measureGap(const juce::Image& icon) {
    // In the mark's units: the slabs leave a gap from y 86 to y 94 where the upper slab's foot
    // (x 24 to 49) stands over the lower slab (x 18 to 81). Checked between x 26 and 47.
    GapReading r;
    const int size = icon.getWidth();
    if (size < 4 || icon.getHeight() != size)
        return r;
    const auto box = markViewBox();
    const float s = static_cast<float>(size) / box.getWidth();
    const auto px = [&](float x) { return (x - box.getX()) * s; };
    const auto py = [&](float y) { return (y - box.getY()) * s; };
    const juce::Image::BitmapData data(icon, juce::Image::BitmapData::readOnly);
    const auto alpha = [&](int x, int y) {
        return reinterpret_cast<const juce::PixelARGB*>(data.getPixelPointer(x, y))->getAlpha() / 255.0f;
    };
    // Columns where the upper slab's foot is flat (x 30 to 46), away from its slanted edges.
    const int firstColumn = static_cast<int>(std::ceil(px(30.0f) - 0.5f)), lastColumn = static_cast<int>(std::floor(px(46.0f) - 0.5f));
    float gap = 0.0f, slabs = 1.0f;
    int gapRows = 0;
    const auto inGap = [&](int y) { return static_cast<float>(y) >= py(86.0f) && static_cast<float>(y + 1) <= py(94.0f); };
    for (int y = 0; y < size; ++y)
        gapRows += inGap(y) ? 1 : 0; // rows wholly inside the gap
    for (int x = std::max(firstColumn, 0); x <= std::min(lastColumn, size - 1); ++x) {
        float above = 0.0f, below = 0.0f; // the strongest coverage of each slab beside the gap
        for (int y = 0; y < size; ++y) {
            const float top = static_cast<float>(y), bottom = top + 1.0f;
            if (inGap(y))
                gap = std::max(gap, alpha(x, y));
            else if (bottom > py(70.0f) && top < py(86.0f))
                above = std::max(above, alpha(x, y));
            else if (bottom > py(94.0f) && top < py(105.0f))
                below = std::max(below, alpha(x, y));
        }
        slabs = std::min({slabs, above, below});
    }
    r.gap = gap;
    r.slabs = slabs;
    r.gapRows = gapRows;
    return r;
}

juce::MemoryBlock iconFile(const std::vector<juce::Image>& images) {
    juce::MemoryOutputStream out;
    const auto u16 = [&](int v) { out.writeShort(static_cast<short>(v)); };
    const auto u32 = [&](juce::uint32 v) { out.writeInt(static_cast<int>(v)); };
    std::vector<juce::MemoryBlock> entries;
    for (const juce::Image& image : images) {
        juce::MemoryOutputStream data;
        const int w = image.getWidth(), h = image.getHeight();
        if (w >= 256) {
            juce::PNGImageFormat png;
            png.writeImageToStream(image, data);
        } else {
            // A 32-bit DIB: the header (twice the height: colour, then the mask), straight-alpha
            // BGRA rows from the bottom, then a 1-bit mask (set where nothing is drawn).
            const int maskRow = ((w + 31) / 32) * 4;
            data.writeInt(40);
            data.writeInt(w);
            data.writeInt(h * 2);
            data.writeShort(1);
            data.writeShort(32);
            data.writeInt(0);
            data.writeInt(w * h * 4 + maskRow * h);
            for (int i = 0; i < 4; ++i)
                data.writeInt(0);
            const juce::Image::BitmapData px(image, juce::Image::BitmapData::readOnly);
            for (int y = h - 1; y >= 0; --y) {
                for (int x = 0; x < w; ++x) {
                    juce::PixelARGB p = *reinterpret_cast<const juce::PixelARGB*>(px.getPixelPointer(x, y));
                    p.unpremultiply();
                    data.writeByte(static_cast<char>(p.getBlue()));
                    data.writeByte(static_cast<char>(p.getGreen()));
                    data.writeByte(static_cast<char>(p.getRed()));
                    data.writeByte(static_cast<char>(p.getAlpha()));
                }
            }
            for (int y = h - 1; y >= 0; --y) {
                std::vector<juce::uint8> row(static_cast<std::size_t>(maskRow), 0);
                for (int x = 0; x < w; ++x)
                    if (reinterpret_cast<const juce::PixelARGB*>(px.getPixelPointer(x, y))->getAlpha() == 0)
                        row[static_cast<std::size_t>(x / 8)] |= static_cast<juce::uint8>(0x80 >> (x % 8));
                data.write(row.data(), row.size());
            }
        }
        entries.push_back(data.getMemoryBlock());
    }
    u16(0); // reserved
    u16(1); // icon
    u16(static_cast<int>(images.size()));
    juce::uint32 offset = 6 + 16 * static_cast<juce::uint32>(images.size());
    for (std::size_t i = 0; i < images.size(); ++i) {
        const int w = images[i].getWidth(), h = images[i].getHeight();
        out.writeByte(static_cast<char>(w >= 256 ? 0 : w));
        out.writeByte(static_cast<char>(h >= 256 ? 0 : h));
        out.writeByte(0); // no palette
        out.writeByte(0);
        u16(1);  // planes
        u16(32); // bits per pixel
        u32(static_cast<juce::uint32>(entries[i].getSize()));
        u32(offset);
        offset += static_cast<juce::uint32>(entries[i].getSize());
    }
    for (const auto& e : entries)
        out.write(e.getData(), e.getSize());
    return out.getMemoryBlock();
}

juce::Image installerSideImage(const Logo& logo, int width, int height) {
    juce::Image image(juce::Image::ARGB, width, height, true, juce::SoftwareImageType());
    juce::Graphics g(image);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff101819u), 0.0f, 0.0f, juce::Colour(0xff1b2527u), 0.0f,
                                           static_cast<float>(height), false));
    g.fillAll();
    if (logo.drawable == nullptr)
        return image;
    // The mark, large, a little above the middle.
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    const float side = w * 0.6f;
    logo.drawable->draw(g, 1.0f, juce::AffineTransform::scale(side / logo.viewport).translated((w - side) * 0.5f, h * 0.38f - side * 0.5f));
    // Below it the wordmark as the header shows it: the mark as the Z, then the letters.
    const Wordmark word = wordmark();
    const juce::Path letters = word.letters(), small = mark();
    juce::Path inked;
    wordmarkStroke().createStrokedPath(inked, letters);
    const auto beside = markBesideLetters(word);
    const auto whole = inked.getBounds().getUnion(small.getBoundsTransformed(beside));
    const float scale = w * 0.7f / whole.getWidth();
    const auto place = juce::AffineTransform::translation(-whole.getX(), -whole.getY())
                           .scaled(scale)
                           .translated((w - whole.getWidth() * scale) * 0.5f, h * 0.7f);
    g.setColour(markColour());
    g.fillPath(small, beside.followedBy(place));
    g.setColour(juce::Colour(0xfff4f1e7u));
    g.fillPath(inked, place);
    return image;
}

juce::Image installerSmallImage(const Logo& logo, int size) {
    juce::Image image(juce::Image::ARGB, size, size, true, juce::SoftwareImageType());
    if (logo.drawable == nullptr)
        return image;
    juce::Graphics g(image);
    const float side = static_cast<float>(size) * 0.92f, offset = (static_cast<float>(size) - side) * 0.5f;
    logo.drawable->draw(g, 1.0f, juce::AffineTransform::scale(side / logo.viewport).translated(offset, offset));
    return image;
}

} // namespace zui::brand
