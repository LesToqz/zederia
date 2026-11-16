// Golden-audio renders (plan 6.1): reference patches rendered with fixed notes and seed, compared
// with stored fingerprints: the level of every 2048-sample frame per channel, and a twelve-band
// spectrum of every 8192 samples. A change to any sound shows up here; the tolerances only
// absorb differences of floating-point rounding between builds.
//
// To accept a deliberate change: set ZED_UPDATE_GOLDEN=1 and run this test once; the files in
// tests/data/golden/ are rewritten (review the diff before committing it).
#include "../support/Analysis.h"
#include "../support/TestUtil.h"
#include "zederia/dsp/Fft.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/render/OfflineRenderer.h"
#include "zederia/util/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>

using namespace zed;

namespace {

constexpr int kFrame = 2048, kBlock = 8192, kBands = 12;

struct Fingerprint {
    std::vector<double> levelL, levelR; // dB per frame
    std::vector<std::array<double, kBands>> bands; // dB per block, octave bands from 40 Hz
    std::uint64_t hash = 0;
};

bool environmentSet(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t length = 0;
    const bool set = _dupenv_s(&value, &length, name) == 0 && value != nullptr;
    std::free(value);
    return set;
#else
    return std::getenv(name) != nullptr;
#endif
}

double db(double x) { return 20.0 * std::log10(std::max(x, 1e-9)); }

Fingerprint fingerprint(const RenderResult& r) {
    Fingerprint f;
    for (std::size_t at = 0; at + kFrame <= r.left.size(); at += kFrame) {
        f.levelL.push_back(std::round(db(zedtest::rmsOf(r.left, at, at + kFrame)) * 1000.0) / 1000.0);
        f.levelR.push_back(std::round(db(zedtest::rmsOf(r.right, at, at + kFrame)) * 1000.0) / 1000.0);
    }
    for (std::size_t at = 0; at + kBlock <= r.left.size(); at += kBlock) {
        std::vector<std::complex<double>> data(kBlock);
        for (int i = 0; i < kBlock; ++i) {
            const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (kBlock - 1));
            data[static_cast<std::size_t>(i)] = 0.5 * (r.left[at + static_cast<std::size_t>(i)] + r.right[at + static_cast<std::size_t>(i)]) * hann;
        }
        dsp::Fft::forSize(kBlock).forward(data.data());
        std::array<double, kBands> bands{};
        for (int b = 0; b < kBands; ++b) {
            const double lo = 40.0 * std::exp2(b), hi = lo * 2.0;
            double power = 0.0;
            for (int k = static_cast<int>(lo / (r.sampleRate / kBlock)); k < static_cast<int>(hi / (r.sampleRate / kBlock)) && k <= kBlock / 2; ++k)
                power += std::norm(data[static_cast<std::size_t>(k)]);
            bands[static_cast<std::size_t>(b)] = std::round(10.0 * std::log10(std::max(power, 1e-18)) * 1000.0) / 1000.0;
        }
        f.bands.push_back(bands);
    }
    f.hash = r.hash();
    return f;
}

nlohmann::json toJson(const Fingerprint& f) {
    nlohmann::json j;
    j["level_left_db"] = f.levelL;
    j["level_right_db"] = f.levelR;
    j["bands_db"] = f.bands;
    j["hash_of_this_build"] = std::to_string(f.hash);
    return j;
}

struct Reference {
    const char* patch;
    const char* notes;
};

const Reference kReferences[] = {
    {"bass", "C2:0:0.3, C2:0.4:0.3:90, G2:0.8:0.6:110"},
    {"pad", "C3:0:2.5, E3:0:2.5:90, G3:0.2:2.3:80, B3:0.4:2.1:70"},
    {"pluck", "C4:0:0.2, E4:0.25:0.2:90, G4:0.5:0.2:100, C5:0.75:0.4:110"},
    {"lead", "A3:0:0.6, C4:0.5:0.5:100, E4:1.0:1.0:110"},
    {"arp_fx", "A2:0:2, C3:0:2, E3:0:2"},
    {"glass_pluck", "C3:0:0.4, E3:0.2:0.4:90, G3:0.4:0.4:110, C4:0.6:0.6"},
};

} // namespace

TEST_CASE("golden: reference patches sound as they did", "[golden]") {
    const bool update = environmentSet("ZED_UPDATE_GOLDEN");
    for (const Reference& ref : kReferences) {
        INFO(ref.patch);
        const LoadResult loaded = loadPatch(zedtest::readText(zedtest::dataPath(std::string("patches/") + ref.patch + ".zpreset")));
        REQUIRE(loaded.ok());
        std::vector<NoteSpec> notes;
        std::string error;
        REQUIRE(parseNoteList(ref.notes, notes, error));
        RenderSettings settings;
        settings.seed = 5;
        settings.tailSeconds = 1.5;
        const RenderResult r = renderOffline(loaded.patch, sequenceFromNotes(notes, settings.sampleRate), settings);
        const Fingerprint now = fingerprint(r);
        const std::string path = zedtest::dataPath(std::string("golden/") + ref.patch + ".json");
        if (update) {
            std::ofstream(path) << toJson(now).dump(1) << "\n";
            WARN("golden file written: " << path);
            continue;
        }
        std::ifstream in(path);
        INFO("missing " << path << ": run this test with ZED_UPDATE_GOLDEN=1 to create it");
        REQUIRE(in.good());
        const auto stored = nlohmann::json::parse(in);
        const auto levelL = stored["level_left_db"].get<std::vector<double>>();
        const auto levelR = stored["level_right_db"].get<std::vector<double>>();
        const auto bands = stored["bands_db"].get<std::vector<std::array<double, kBands>>>();
        REQUIRE(levelL.size() == now.levelL.size());
        REQUIRE(bands.size() == now.bands.size());
        double worstLevel = 0.0, worstBand = 0.0;
        for (std::size_t i = 0; i < levelL.size(); ++i) {
            if (levelL[i] > -90.0)
                worstLevel = std::max(worstLevel, std::abs(levelL[i] - now.levelL[i]));
            if (levelR[i] > -90.0)
                worstLevel = std::max(worstLevel, std::abs(levelR[i] - now.levelR[i]));
        }
        for (std::size_t i = 0; i < bands.size(); ++i)
            for (int b = 0; b < kBands; ++b)
                if (bands[i][static_cast<std::size_t>(b)] > -80.0)
                    worstBand = std::max(worstBand, std::abs(bands[i][static_cast<std::size_t>(b)] - now.bands[i][static_cast<std::size_t>(b)]));
        INFO("largest level difference " << worstLevel << " dB, largest band difference " << worstBand << " dB");
        CHECK(worstLevel < 0.05);
        CHECK(worstBand < 0.1);
    }
}
