// Helpers shared by the core tests.
#pragma once

#include "zederia/engine/Engine.h"
#include "zederia/patch/Patch.h"
#include "zederia/render/OfflineRenderer.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace zedtest {

inline std::string dataPath(const std::string& name) { return std::string(ZED_TEST_DATA_DIR) + "/" + name; }

inline std::string readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    std::string text = out.str();
    std::erase(text, '\r');
    return text;
}

inline int idx(const char* id) { return zed::Registry::get().indexOf(id); }

// Frequency of a signal from its upward zero crossings, each located by linear interpolation.
// Returns 0 when fewer than two crossings are found in [from, to).
inline double measureFrequency(const std::vector<float>& x, std::size_t from, std::size_t to, double sampleRate) {
    double first = -1.0, last = -1.0;
    int count = 0;
    for (std::size_t i = from; i + 1 < to && i + 1 < x.size(); ++i) {
        if (x[i] < 0.0f && x[i + 1] >= 0.0f) {
            const double t = static_cast<double>(i) + static_cast<double>(-x[i]) / (static_cast<double>(x[i + 1]) - x[i]);
            if (count == 0)
                first = t;
            last = t;
            ++count;
        }
    }
    return count < 2 ? 0.0 : (count - 1) / (last - first) * sampleRate;
}

inline double cents(double measured, double expected) { return 1200.0 * std::log2(measured / expected); }

inline float peakOf(const std::vector<float>& x, std::size_t from, std::size_t to) {
    float peak = 0.0f;
    for (std::size_t i = from; i < to && i < x.size(); ++i)
        peak = std::max(peak, std::abs(x[i]));
    return peak;
}

// Renders one held note and returns the result.
inline zed::RenderResult renderNote(const zed::Patch& patch, int note, double seconds, double sampleRate = 48000.0,
                                    double velocity = 1.0) {
    const zed::NoteSpec notes[] = {{note, 0.0, seconds, velocity}};
    const auto events = zed::sequenceFromNotes(notes, sampleRate);
    zed::RenderSettings settings;
    settings.sampleRate = sampleRate;
    return zed::renderOffline(patch, events, settings);
}

} // namespace zedtest
