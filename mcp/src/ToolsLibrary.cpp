#include "Tools.h"

#include "zederia/audio/WavFile.h"
#include "zederia/content/PresetIndex.h"
#include "zederia/content/Recipe.h"
#include "zederia/table/Formula.h"
#include "zederia/table/Import.h"
#include "zederia/table/Toolkit.h"
#include "zederia/util/Compress.h"
#include "zederia/util/Image.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace zmcp {

using zed::Patch;

namespace {

std::vector<std::uint8_t> readBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void writeBytes(const fs::path& p, const std::vector<std::uint8_t>& bytes, const std::string& argPath) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f)
        fail(argPath, "the file " + utf8(p) + " could not be written", "check the folder is writable and the disk has room");
}

// A name that is safe as a file name: no separators, devices or reserved characters.
void checkFileName(const std::string& name, const std::string& path) {
    if (name.empty() || name.size() > 80)
        fail(path, "the name must have 1 to 80 characters", "give a short name such as \"Glass Pluck\"");
    for (const char c : name)
        if (std::string("/\\:*?\"<>|").find(c) != std::string::npos || static_cast<unsigned char>(c) < 32)
            fail(path, "the name contains \"" + std::string(1, c) + "\", which cannot be part of a file name", "use letters, digits, spaces, - and _");
    if (name.front() == ' ' || name.back() == ' ' || name.back() == '.' || name == "." || name == "..")
        fail(path, "the name cannot start or end with a space or end with a dot", "trim it");
}

J tableStats(const zed::Wavetable& t) {
    float peak = 0.0f;
    double sum = 0.0, dc = 0.0;
    for (const auto& f : t.frames) {
        peak = std::max(peak, zed::framePeak(f));
        for (const float s : f) {
            sum += static_cast<double>(s) * s;
            dc += s;
        }
    }
    const double n = static_cast<double>(t.frames.size()) * zed::kFrameSize;
    const auto bright = [](const zed::Frame& f) {
        const auto h = zed::analyzeFrame(f);
        double num = 0.0, den = 0.0;
        for (int k = 1; k <= zed::kMaxHarmonics; ++k) {
            num += k * h.magnitude[static_cast<std::size_t>(k)];
            den += h.magnitude[static_cast<std::size_t>(k)];
        }
        return den > 0.0 ? num / den : 0.0;
    };
    return {{"peak", std::round(peak * 1000.0) / 1000.0},
            {"rms_db", n > 0 && sum > 0 ? std::round(100.0 * 10.0 * std::log10(sum / n)) / 100.0 : -200.0},
            {"dc", std::round(dc / std::max(1.0, n) * 1e5) / 1e5},
            {"brightness_first", std::round(bright(t.frames.front()) * 100.0) / 100.0},
            {"brightness_last", std::round(bright(t.frames.back()) * 100.0) / 100.0},
            {"brightness_note", "mean harmonic number weighted by level: 1 is a sine, higher is brighter"}};
}

// A small picture of the first, middle and last frames.
std::vector<std::uint8_t> tableThumbnail(const zed::Wavetable& t) {
    const int w = 256, h = 96;
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w * h * 3), 18);
    const int picks[3] = {0, t.size() / 2, t.size() - 1};
    const std::uint8_t colours[3][3] = {{96, 196, 255}, {200, 140, 255}, {255, 170, 80}};
    for (int c = 0; c < 3; ++c) {
        const auto& f = t.frames[static_cast<std::size_t>(picks[c])];
        int lastY = -1;
        for (int x = 0; x < w; ++x) {
            const float s = f[static_cast<std::size_t>(x * zed::kFrameSize / w)];
            const int y = std::clamp(static_cast<int>(std::lround((1.0 - (static_cast<double>(s) + 1.0) * 0.5) * (h - 1))), 0, h - 1);
            const int from = lastY < 0 ? y : std::min(y, lastY), to = lastY < 0 ? y : std::max(y, lastY);
            for (int yy = from; yy <= to; ++yy)
                for (int ch = 0; ch < 3; ++ch)
                    rgb[static_cast<std::size_t>((yy * w + x) * 3 + ch)] = colours[c][ch];
            lastY = y;
        }
    }
    return zed::encodePngRgb(w, h, rgb);
}

zed::Frame shapeFrame(const std::string& name, const std::string& path) {
    std::string expr;
    if (name == "sine") expr = "sin(tau*t)";
    else if (name == "triangle") expr = "tri(t)";
    else if (name == "saw") expr = "saw(t)";
    else if (name == "square") expr = "square(t)";
    else if (name.rfind("pulse", 0) == 0) {
        const int pct = name.size() > 5 ? std::atoi(name.c_str() + 5) : 25;
        if (pct < 1 || pct > 99)
            fail(path, "\"" + name + "\" needs a width from 1 to 99", "for example \"pulse25\"");
        expr = "t < " + std::to_string(pct / 100.0) + " ? 1 : -1";
    } else if (name == "half_sine") expr = "max(sin(tau*t), 0) * 2 - 0.6366";
    else
        fail(path, "unknown shape \"" + name + "\"", "use sine, triangle, saw, square, pulseNN (1..99) or half_sine");
    zed::Wavetable one = zed::Wavetable::silent(1);
    zed::FormulaRequest req;
    req.expression = expr;
    std::string error;
    zed::applyFormula(one, req, error);
    return one.frames.front();
}

// Writes the table to the user's Wavetables/Agent folder and returns its reference.
std::string saveTable(Bridge& bridge, const zed::Wavetable& t, const std::string& name, J& out) {
    const fs::path file = bridge.resolveWrite(name + ".wav", {".wav"}, "/name", bridge.userFolder("Wavetables/Agent"));
    writeBytes(file, zed::encodeWavetable(t), "/name");
    out["file"] = utf8(file);
    return "user:Agent/" + name;
}

void loadIntoTarget(Bridge& bridge, Call& call, const J& a, const std::shared_ptr<const zed::Wavetable>& table, const std::string& ref,
                    J& out) {
    if (!a.contains("target"))
        return;
    const std::string target = argString(a, "target");
    bridge.checkTarget(target);
    const int osc = argInt(a, "osc", 1);
    if (osc < 1 || osc > zed::kNumOscs)
        fail("/osc", "osc must be 1, 2 or 3", "say which oscillator plays the table");
    bridge.edit(target, call, "Table on Osc " + std::to_string(osc) + ": " + table->name, [&](Patch& p) {
        const auto i = static_cast<std::size_t>(osc - 1);
        p.tables[i] = ref;
        p.tableData[i].table = table; // kept in the patch so every instance plays it the same
        p.set("osc" + std::to_string(osc) + ".on", 1.0);
        p.set("osc" + std::to_string(osc) + ".engine", 0.0); // wavetable
    });
    out["loaded_into"] = {{"target", target}, {"osc", osc}};
}

ToolOutput tableResult(Bridge& bridge, Call& call, const J& a, zed::Wavetable table, const std::string& name, bool save) {
    ToolOutput out;
    table.name = name;
    J s = {{"name", name}, {"frames", table.size()}, {"morph", std::string(zed::morphModeName(table.morph))}, {"stepped", table.stepped}};
    std::string ref = "embedded:" + name;
    if (save) {
        ref = saveTable(bridge, table, name, s);
        bridge.report(call, "saved the wavetable " + ref);
    }
    s["table_id"] = ref;
    s["stats"] = tableStats(table);
    auto shared = std::make_shared<const zed::Wavetable>(table);
    loadIntoTarget(bridge, call, a, shared, save ? ref : std::string(), s);
    if (argBool(a, "thumbnail", false)) {
        const auto png = tableThumbnail(table);
        out.content.push_back({{"type", "image"}, {"mimeType", "image/png"}, {"data", zed::base64Encode(png)}});
    }
    out.structured = s;
    out.summary = "Table \"" + name + "\": " + std::to_string(table.size()) + " frame(s), " + std::string(zed::morphModeName(table.morph)) +
                  (save ? ", saved as " + ref : "") + (s.contains("loaded_into") ? ", loaded into osc " + std::to_string(argInt(a, "osc", 1)) : "") + ".";
    return out;
}

void applyCommon(zed::Wavetable& t, const J& a) {
    if (a.contains("morph")) {
        if (!zed::morphModeFromName(argString(a, "morph"), t.morph))
            fail("/morph", "unknown morph mode", "use none, crossfade, spectral, spectral_aligned or spectral_zero");
    } else if (t.size() > 1 && t.morph == zed::MorphMode::None) {
        t.morph = zed::MorphMode::Crossfade;
    }
    if (a.contains("stepped"))
        t.stepped = argBool(a, "stepped", false);
    if (argBool(a, "normalize", true))
        zed::processFrames(t, zed::allFrames(), zed::ProcessOp::NormalizeShared);
}

J presetEntry(Bridge& bridge, const zed::PresetInfo& p) {
    return {{"id", bridge.presetId(pathFromUtf8(p.path))}, {"name", p.meta.name}, {"type", p.meta.type}, {"tags", p.meta.tags},
            {"genres", p.meta.genres}, {"author", p.meta.author}, {"pack", p.meta.pack}, {"rating", p.meta.rating},
            {"origin", p.factory ? "factory" : "user"}, {"folder", p.folder}};
}

} // namespace

void addLibraryTools(std::vector<ToolDef>& tools, Bridge& bridge) {
    // --- create_wavetable ---
    tools.push_back({"create_wavetable", "Create a wavetable",
                     "Makes a wavetable from harmonic levels, a formula (Appendix B language: t, x, y, z, h, sin, saw, ...), raw frames, "
                     "basic shapes, an audio file or an image (inside allowed folders), or recipe steps. Saved to the user's "
                     "Wavetables/Agent folder (save: false keeps it only in the target). With target and osc it is loaded into that "
                     "oscillator as one undo step. Returns the table id and statistics, and a thumbnail image on request.",
                     schema(R"json({"type":"object","additionalProperties":false,"required":["name","kind"],"properties":{
                        "name":{"type":"string","minLength":1,"maxLength":80},
                        "kind":{"enum":["harmonics","formula","frames","shapes","audio","image","recipe"]},
                        "harmonics":{"type":"array","maxItems":256,"description":"levels of harmonics 1, 2, 3 ... (one frame), or a list of such lists (one per key frame)"},
                        "phases":{"type":"array","maxItems":256,"description":"optional phases in radians, the same layout as harmonics"},
                        "expression":{"type":"string","maxLength":4096,"description":"formula: the sample value (wave) or harmonic level (harmonics)"},
                        "phase_expression":{"type":"string","maxLength":4096},
                        "domain":{"enum":["wave","harmonics"]},
                        "frames":{"type":"array","maxItems":256,"items":{"type":"array","minItems":16,"maxItems":8192,"items":{"type":"number"}},"description":"raw cycles, each resampled to 2048 samples"},
                        "shapes":{"type":"array","minItems":1,"maxItems":256,"items":{"type":"string"},"description":"sine, triangle, saw, square, pulse25, half_sine: one key frame each"},
                        "path":{"type":"string","description":"audio (.wav) or image (.png, .bmp, .pgm, .ppm) file"},
                        "method":{"enum":["single_cycle","pitch_tracked","fixed","spectral"],"description":"how audio becomes frames"},
                        "image_mode":{"enum":["amplitude","harmonics"]},
                        "steps":{"type":"array","maxItems":64,"items":{"type":"object"},"description":"recipe steps, as in edit_wavetable"},
                        "frame_count":{"type":"integer","minimum":1,"maximum":256,"description":"key frames for formula and recipe tables, the most for audio and images"},
                        "morph":{"enum":["none","crossfade","spectral","spectral_aligned","spectral_zero"]},
                        "stepped":{"type":"boolean"},
                        "normalize":{"type":"boolean","description":"scale so the loudest frame peaks at 1 (default true)"},
                        "save":{"type":"boolean","description":"write the table file (default true)"},
                        "target":{"type":"string","description":"load it into this target"},
                        "osc":{"type":"integer","minimum":1,"maximum":3},
                        "thumbnail":{"type":"boolean"}}})json"),
                     schema(R"json({"type":"object","required":["table_id","name","frames","morph","stats"],"properties":{"table_id":{"type":"string"},
                        "name":{"type":"string"},"frames":{"type":"integer"},"morph":{"type":"string"},"stats":{"type":"object"},"file":{"type":"string"},"loaded_into":{"type":"object"}}})json"),
                     false, false, false, [&bridge](const J& a, Call& call) {
                         const std::string name = argString(a, "name");
                         checkFileName(name, "/name");
                         const std::string kind = argString(a, "kind");
                         const int count = argInt(a, "frame_count", 1);
                         zed::Wavetable t;
                         std::string error;
                         if (kind == "harmonics") {
                             if (!a.contains("harmonics") || a["harmonics"].empty())
                                 fail("/harmonics", "kind harmonics needs harmonic levels", "give [1, 0.5, 0.33, ...] for one frame");
                             J frames = a["harmonics"];
                             if (!frames[0].is_array())
                                 frames = J::array({frames});
                             const J phases = a.value("phases", J::array());
                             for (std::size_t f = 0; f < frames.size(); ++f) {
                                 if (!frames[f].is_array() || frames[f].size() > static_cast<std::size_t>(zed::kMaxHarmonics))
                                     fail("/harmonics/" + std::to_string(f), "each frame is a list of at most 1024 levels", "give numbers for harmonics 1, 2, 3 ...");
                                 zed::Harmonics h;
                                 for (std::size_t k = 0; k < frames[f].size(); ++k) {
                                     if (!frames[f][k].is_number())
                                         fail("/harmonics/" + std::to_string(f) + "/" + std::to_string(k), "a level must be a number", "use 0 for silent harmonics");
                                     h.magnitude[k + 1] = std::abs(frames[f][k].get<double>());
                                     const J ph = phases.is_array() && f < phases.size() ? (phases[f].is_array() ? phases[f] : phases) : J();
                                     if (ph.is_array() && k < ph.size() && ph[k].is_number())
                                         h.phase[k + 1] = ph[k].get<double>();
                                 }
                                 t.frames.push_back(zed::synthesizeFrame(h));
                             }
                         } else if (kind == "formula") {
                             if (argString(a, "expression").empty())
                                 fail("/expression", "kind formula needs an expression", "for example \"sin(tau*t) * (1 - y) + saw(t) * y\"");
                             t = zed::Wavetable::silent(count);
                             zed::FormulaRequest req;
                             req.domain = argString(a, "domain", "wave") == "harmonics" ? zed::FormulaDomain::Harmonics : zed::FormulaDomain::Wave;
                             req.expression = argString(a, "expression");
                             req.phaseExpression = argString(a, "phase_expression");
                             if (!zed::applyFormula(t, req, error))
                                 fail("/expression", error, "check the expression against the formula language in zederia://guide/agent");
                         } else if (kind == "frames") {
                             const J frames = a.value("frames", J::array());
                             if (frames.empty())
                                 fail("/frames", "kind frames needs at least one cycle", "give lists of samples, one cycle each");
                             for (const auto& f : frames) {
                                 std::vector<float> cyc;
                                 for (const auto& s : f)
                                     cyc.push_back(static_cast<float>(std::clamp(s.get<double>(), -4.0, 4.0)));
                                 t.frames.push_back(zed::resampleCycle(cyc));
                             }
                         } else if (kind == "shapes") {
                             const J shapes = a.value("shapes", J::array());
                             if (shapes.empty())
                                 fail("/shapes", "kind shapes needs shape names", "for example [\"sine\", \"saw\"]");
                             for (std::size_t i = 0; i < shapes.size(); ++i)
                                 t.frames.push_back(shapeFrame(shapes[i].get<std::string>(), "/shapes/" + std::to_string(i)));
                         } else if (kind == "audio") {
                             const fs::path file = bridge.resolveRead(argString(a, "path"), {".wav"}, "/path");
                             zed::WavData wav;
                             if (!zed::decodeWav(readBytes(file), wav, error))
                                 fail("/path", "the audio file cannot be read: " + error, "use a PCM or float WAV file");
                             const auto clip = zed::clipFromWav(wav, zed::StereoChoice::Mid);
                             const std::string method = argString(a, "method", "pitch_tracked");
                             zed::SliceOptions so;
                             so.maxFrames = a.contains("frame_count") ? count : 64;
                             bool ok = false;
                             if (method == "single_cycle") {
                                 ok = zed::importSingleCycles(std::span<const zed::AudioClip>(&clip, 1), t, error);
                             } else if (method == "fixed") {
                                 ok = zed::importFixedCycles(clip, so, t, error);
                             } else if (method == "spectral") {
                                 zed::SnapshotOptions sn;
                                 sn.frames = so.maxFrames;
                                 ok = zed::importSpectralSnapshots(clip, sn, t, error);
                             } else {
                                 ok = zed::importPitchTracked(clip, so, t, error);
                             }
                             if (!ok)
                                 fail("/method", "the import failed: " + error, "try method \"spectral\" for unpitched material or \"single_cycle\" for one cycle");
                         } else if (kind == "image") {
                             const fs::path file = bridge.resolveRead(argString(a, "path"), {".png", ".bmp", ".pgm", ".ppm", ".pnm"}, "/path");
                             zed::ImageData img;
                             if (!zed::decodeImage(readBytes(file), img, error))
                                 fail("/path", "the image cannot be read: " + error, "use a PNG, BMP or PGM/PPM file");
                             const auto mode = argString(a, "image_mode", "amplitude") == "harmonics" ? zed::ImageMode::Harmonics : zed::ImageMode::Amplitude;
                             if (!zed::importImage(img, mode, a.contains("frame_count") ? count : 64, t, error))
                                 fail("/path", "the image import failed: " + error, "use a smaller or simpler image");
                         } else if (kind == "recipe") {
                             J recipe = {{"kind", "table"}, {"name", name}, {"frames", count}, {"steps", a.value("steps", J::array())}};
                             if (!zed::buildTableRecipe(recipe, t, error))
                                 fail("/steps", error, "see the recipe steps in zederia://guide/agent");
                         }
                         if (t.frames.empty() || t.size() > zed::kMaxFrames)
                             fail("", "the table must have 1 to 256 frames", "give fewer frames");
                         applyCommon(t, a);
                         return tableResult(bridge, call, a, std::move(t), name, argBool(a, "save", true));
                     }});

    // --- edit_wavetable ---
    tools.push_back({"edit_wavetable", "Edit a wavetable",
                     "Applies operations to a table: recipe steps such as {\"op\":\"process\",\"name\":\"normalize_each\"}, "
                     "{\"op\":\"harmonic\",\"name\":\"odd_only\"}, {\"op\":\"formula\",\"domain\":\"wave\",\"expr\":\"in*0.5+sin(tau*t)*0.5\"}, "
                     "{\"op\":\"morph\",\"mode\":\"spectral\"}, {\"op\":\"sort\",\"by\":\"brightness\"}, {\"op\":\"frames\",\"count\":32}, "
                     "{\"op\":\"duplicate\"}, {\"op\":\"reverse\"} (each may limit itself with \"frames\": [first, last]). The table is "
                     "table_id (from list_wavetables or create_wavetable) or the one an oscillator of target plays. A user table is saved "
                     "in place unless save_as names a new one; factory tables are saved under a new name.",
                     schema(R"json({"type":"object","additionalProperties":false,"required":["operations"],"properties":{
                        "table_id":{"type":"string","description":"\"factory:Group/Name\" or \"user:Folder/Name\""},
                        "target":{"type":"string","description":"edit the table this target's oscillator plays (with osc), and load the result back"},
                        "osc":{"type":"integer","minimum":1,"maximum":3},
                        "operations":{"type":"array","minItems":1,"maxItems":64,"items":{"type":"object","required":["op"]}},
                        "save_as":{"type":"string","maxLength":80},
                        "save":{"type":"boolean","description":"write the result (default true)"},
                        "thumbnail":{"type":"boolean"}}})json"),
                     schema(R"json({"type":"object","required":["table_id","name","frames","morph","stats"],"properties":{"table_id":{"type":"string"},
                        "name":{"type":"string"},"frames":{"type":"integer"},"morph":{"type":"string"},"stats":{"type":"object"},"file":{"type":"string"},"loaded_into":{"type":"object"}}})json"),
                     false, true, false, [&bridge](const J& a, Call& call) {
                         zed::Wavetable t;
                         std::string name, error, ref = argString(a, "table_id");
                         if (!ref.empty()) {
                             const std::string path = bridge.library().tablePath(ref);
                             std::vector<std::uint8_t> bytes;
                             if (path.empty() || !zed::readFile(path, bytes, error) || !zed::decodeWavetable(bytes, t, error))
                                 fail("/table_id", "no readable table \"" + ref + "\"", "use an id from list_wavetables");
                             name = t.name.empty() ? ref.substr(ref.rfind('/') + 1) : t.name;
                         } else if (a.contains("target")) {
                             const std::string target = argString(a, "target");
                             bridge.checkTarget(target);
                             const int osc = argInt(a, "osc", 1);
                             const Patch p = bridge.patch(target, call);
                             const auto i = static_cast<std::size_t>(std::clamp(osc, 1, 3) - 1);
                             if (p.tableData[i]) {
                                 t = *p.tableData[i].table;
                             } else if (!p.tables[i].empty()) {
                                 std::vector<std::uint8_t> bytes;
                                 const std::string path = bridge.library().tablePath(p.tables[i]);
                                 if (path.empty() || !zed::readFile(path, bytes, error) || !zed::decodeWavetable(bytes, t, error))
                                     fail("/target", "the oscillator's table \"" + p.tables[i] + "\" cannot be read", "load another table first");
                             } else {
                                 t = zed::TableData::builtinWavetable();
                             }
                             ref = p.tables[i];
                             name = t.name.empty() ? "Osc " + std::to_string(osc) + " table" : t.name;
                         } else {
                             fail("/table_id", "say which table to edit", "give table_id, or target and osc");
                         }
                         if (!zed::applyTableSteps(a["operations"], t, error))
                             fail("/operations", error, "see the operation list in this tool's description and zederia://guide/agent");
                         std::string saveName = argString(a, "save_as", "");
                         if (saveName.empty())
                             saveName = ref.rfind("user:Agent/", 0) == 0 ? ref.substr(11) : name + " edit";
                         checkFileName(saveName, "/save_as");
                         return tableResult(bridge, call, a, std::move(t), saveName, argBool(a, "save", true));
                     }});

    // --- list_wavetables ---
    tools.push_back({"list_wavetables", "List wavetables",
                     "Factory and user wavetables by id (\"factory:Basics/Saw Morph\"), with their category. search matches words in the "
                     "name or category.",
                     schema(R"json({"type":"object","additionalProperties":false,"properties":{
                        "search":{"type":"string"},"category":{"type":"string"},"limit":{"type":"integer","minimum":1,"maximum":500}}})json"),
                     schema(R"json({"type":"object","required":["tables","total"],"properties":{"total":{"type":"integer"},
                        "tables":{"type":"array","items":{"type":"object","required":["id","name","category","origin"]}}}})json"),
                     true, false, true, [&bridge](const J& a, Call&) {
                         const auto refs = bridge.library().listTables();
                         std::string search = argString(a, "search"), category = argString(a, "category");
                         for (auto& c : search)
                             c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                         J list = J::array();
                         int total = 0;
                         for (const auto& r : refs) {
                             const auto colon = r.find(':');
                             const std::string rest = r.substr(colon + 1);
                             const auto slash = rest.rfind('/');
                             const std::string cat = slash == std::string::npos ? "" : rest.substr(0, slash);
                             const std::string nm = slash == std::string::npos ? rest : rest.substr(slash + 1);
                             std::string hay = rest;
                             for (auto& c : hay)
                                 c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                             if (!search.empty() && hay.find(search) == std::string::npos)
                                 continue;
                             if (!category.empty() && cat != category)
                                 continue;
                             ++total;
                             if (static_cast<int>(list.size()) < argInt(a, "limit", 200))
                                 list.push_back({{"id", r}, {"name", nm}, {"category", cat}, {"origin", r.substr(0, colon)}});
                         }
                         ToolOutput out;
                         out.structured = {{"tables", list}, {"total", total}};
                         out.summary = std::to_string(total) + " table(s).";
                         return out;
                     }});

    // --- list_presets ---
    tools.push_back({"list_presets", "List presets",
                     "Browses factory and user presets. search uses the library's query language: free words plus tag:warm type:bass "
                     "genre:house pack:\"Night Drive\" author:me rating:3 fav factory user." + std::string(kUntrustedNote),
                     schema(R"json({"type":"object","additionalProperties":false,"properties":{
                        "search":{"type":"string","maxLength":400},
                        "tags":{"type":"array","items":{"type":"string"}},
                        "type":{"type":"string"},
                        "origin":{"enum":["any","factory","user"]},
                        "limit":{"type":"integer","minimum":1,"maximum":500},
                        "offset":{"type":"integer","minimum":0}}})json"),
                     schema(R"json({"type":"object","required":["presets","total","facets"],"properties":{"total":{"type":"integer"},"facets":{"type":"object"},
                        "presets":{"type":"array","items":{"type":"object","required":["id","name","type","tags","origin"]}}}})json"),
                     true, false, true, [&bridge](const J& a, Call&) {
                         const auto presets = zed::scanPresets(bridge.library().factoryDir(), utf8(bridge.userDir()));
                         std::string query = argString(a, "search");
                         for (const auto& t : a.value("tags", J::array()))
                             if (t.is_string())
                                 query += " tag:\"" + t.get<std::string>() + "\"";
                         if (a.contains("type"))
                             query += " type:\"" + argString(a, "type") + "\"";
                         const std::string origin = argString(a, "origin", "any");
                         if (origin != "any")
                             query += " " + origin;
                         const auto q = zed::parsePresetQuery(query);
                         std::vector<int> items;
                         for (int i = 0; i < static_cast<int>(presets.size()); ++i)
                             if (zed::presetMatches(presets[static_cast<std::size_t>(i)], q))
                                 items.push_back(i);
                         zed::sortPresets(presets, items, zed::PresetSort::Name, false);
                         J list = J::array();
                         const int offset = argInt(a, "offset", 0), limit = argInt(a, "limit", 100);
                         for (int i = offset; i < static_cast<int>(items.size()) && static_cast<int>(list.size()) < limit; ++i)
                             list.push_back(presetEntry(bridge, presets[static_cast<std::size_t>(items[static_cast<std::size_t>(i)])]));
                         J facets = {{"type", zed::presetFacet(presets, items, "type")}, {"tag", zed::presetFacet(presets, items, "tag")}};
                         ToolOutput out;
                         out.structured = {{"presets", list}, {"total", items.size()}, {"facets", facets}};
                         out.summary = std::to_string(items.size()) + " preset(s) match.";
                         return out;
                     }});

    // --- load_preset ---
    tools.push_back({"load_preset", "Load a preset", "Loads a preset into a target as one undo step. A live instance keeps its locked parameters." +
                                                         std::string(kUntrustedNote),
                     schema(R"json({"type":"object","additionalProperties":false,"required":["target","preset"],"properties":{
                        "target":{"type":"string"},"preset":{"type":"string","description":"a preset id from list_presets"}}})json"),
                     schema(R"json({"type":"object","required":["target","preset","applied","summary"],"properties":{"target":{"type":"string"},"preset":{"type":"string"},
                        "applied":{"type":"boolean"},"summary":{"type":"object"}}})json"),
                     false, false, true, [&bridge](const J& a, Call& call) {
                         const std::string t = argString(a, "target"), id = argString(a, "preset");
                         bridge.checkTarget(t);
                         const fs::path file = bridge.presetPath(id);
                         std::vector<std::uint8_t> bytes;
                         std::string error;
                         if (file.empty() || !zed::readFile(utf8(file), bytes, error))
                             fail("/preset", "no preset \"" + id + "\"", "use an id from list_presets");
                         auto loaded = zed::loadPatch(std::string(bytes.begin(), bytes.end()));
                         if (!loaded.ok())
                             fail("/preset", "the preset cannot be read", "choose another preset");
                         auto r = bridge.edit(t, call, "Load preset " + displayName(loaded.patch), [&](Patch& p) { p = loaded.patch; }, false, true);
                         ToolOutput out;
                         out.structured = {{"target", t}, {"preset", id}, {"applied", r.applied}, {"summary", patchSummary(r.after)}};
                         out.summary = "Loaded \"" + displayName(loaded.patch) + "\" into " + t + ".";
                         return out;
                     }});

    // --- save_preset ---
    tools.push_back({"save_preset", "Save a preset",
                     "Saves the target's patch as a preset with its name, tags, type, author and notes, and sets this metadata in the "
                     "target (one undo step). Agents' presets go to the user's Presets/Agent folder (Presets/Agent/<pack> with a pack). "
                     "Save anywhere else in the user's presets (folder) only when the user asked for that folder.",
                     schema(R"json({"type":"object","additionalProperties":false,"required":["target","name"],"properties":{
                        "target":{"type":"string"},
                        "name":{"type":"string","minLength":1,"maxLength":80},
                        "tags":{"type":"array","maxItems":24,"items":{"type":"string","maxLength":40}},
                        "genres":{"type":"array","maxItems":12,"items":{"type":"string","maxLength":40}},
                        "type":{"type":"string","maxLength":40,"description":"bass, lead, pluck, keys, pad, strings, brass, sequence, fx, percussion ..."},
                        "pack":{"type":"string","maxLength":80,"description":"the pack the preset belongs to; it is saved in Presets/Agent/<pack>"},
                        "folder":{"type":"string","maxLength":200,"description":"only when the user asked to save there: a folder inside the user's Presets folder to use instead of Agent, such as \"Bass\" or \"808/Kicks\". Leave it out otherwise."},
                        "author":{"type":"string","maxLength":80},
                        "notes":{"type":"string","maxLength":2000},
                        "overwrite":{"type":"boolean"}}})json"),
                     schema(R"json({"type":"object","required":["preset_id","path","name"],"properties":{"preset_id":{"type":"string"},"path":{"type":"string"},"name":{"type":"string"},"folder":{"type":"string"}}})json"),
                     false, false, true, [&bridge](const J& a, Call& call) {
                         const std::string t = argString(a, "target"), name = argString(a, "name");
                         bridge.checkTarget(t);
                         checkFileName(name, "/name");
                         const std::string pack = argString(a, "pack");
                         if (!pack.empty())
                             checkFileName(pack, "/pack");
                         // Presets/Agent unless the user named another folder (the agent passes it as folder).
                         std::string folder = "Agent" + (pack.empty() ? std::string() : "/" + pack);
                         if (a.contains("folder")) {
                             std::string given = argString(a, "folder");
                             if (given.find(':') != std::string::npos)
                                 fail("/folder", "the folder must be inside the user's Presets folder", "give it relative to Presets, such as \"Bass\" or \"808/Kicks\"");
                             std::replace(given.begin(), given.end(), '\\', '/');
                             std::string clean, part;
                             std::stringstream parts(given);
                             int depth = 0;
                             while (std::getline(parts, part, '/')) {
                                 if (part.empty())
                                     continue;
                                 checkFileName(part, "/folder");
                                 if (++depth > 4)
                                     fail("/folder", "the folder is nested more than four levels deep", "use a shorter path such as \"Bass/Phonk\"");
                                 clean += (clean.empty() ? "" : "/") + part;
                             }
                             if (clean.empty())
                                 fail("/folder", "the folder is empty", "leave folder out to save in Presets/Agent");
                             folder = clean;
                         }
                         const fs::path file = bridge.resolveWrite(folder + "/" + name + ".zpreset", {".zpreset"}, "/name", bridge.userFolder("Presets"));
                         const bool overwrite = argBool(a, "overwrite", false);
                         std::error_code ec;
                         if (fs::exists(file, ec) && !overwrite)
                             fail("/name", "a preset called \"" + name + "\" already exists there", "pass overwrite: true, or choose another name");
                         auto r = bridge.edit(t, call, "Name preset " + name, [&](Patch& p) {
                             p.meta.name = name;
                             if (a.contains("tags")) p.meta.tags = a["tags"].get<std::vector<std::string>>();
                             if (a.contains("genres")) p.meta.genres = a["genres"].get<std::vector<std::string>>();
                             if (a.contains("type")) p.meta.type = argString(a, "type");
                             if (a.contains("author")) p.meta.author = argString(a, "author");
                             if (a.contains("notes")) p.meta.notes = argString(a, "notes");
                             p.meta.pack = pack;
                         });
                         const std::string text = zed::savePatch(r.after);
                         // Without overwrite the file is created only when the name is still free, so a
                         // save of the same name that finished meanwhile is not replaced.
                         bool taken = false;
                         if (!overwrite && !zed::link::createNewFile(utf8(file), taken)) {
                             if (taken)
                                 fail("/name", "a preset called \"" + name + "\" was saved there meanwhile", "pass overwrite: true, or choose another name");
                             fail("/name", "the preset could not be written to " + utf8(file), "check the folder is writable");
                         }
                         std::ofstream f(file, std::ios::binary);
                         f << text;
                         if (!f)
                             fail("/name", "the preset could not be written to " + utf8(file), "check the folder is writable");
                         bridge.report(call, "saved the preset " + bridge.presetId(file), t);
                         ToolOutput out;
                         out.structured = {{"preset_id", bridge.presetId(file)}, {"path", utf8(file)}, {"name", name}, {"folder", folder}};
                         out.summary = "Saved \"" + name + "\" to " + utf8(file) + ".";
                         return out;
                     }});

    // --- tag_presets ---
    tools.push_back({"tag_presets", "Edit preset metadata",
                     "Bulk-edits the metadata of user presets: add, remove or set tags and genres, and set type, pack, author or rating. "
                     "Factory presets are read-only and are reported as skipped.",
                     schema(R"json({"type":"object","additionalProperties":false,"required":["presets"],"properties":{
                        "presets":{"type":"array","minItems":1,"maxItems":500,"items":{"type":"string"}},
                        "add_tags":{"type":"array","items":{"type":"string","maxLength":40}},
                        "remove_tags":{"type":"array","items":{"type":"string"}},
                        "set_tags":{"type":"array","items":{"type":"string","maxLength":40}},
                        "genres":{"type":"array","items":{"type":"string","maxLength":40}},
                        "type":{"type":"string","maxLength":40},"pack":{"type":"string","maxLength":80},"author":{"type":"string","maxLength":80},
                        "rating":{"type":"integer","minimum":0,"maximum":5}}})json"),
                     schema(R"json({"type":"object","required":["changed","skipped"],"properties":{"changed":{"type":"integer"},
                        "skipped":{"type":"array","items":{"type":"object","required":["preset","reason"]}}}})json"),
                     false, true, true, [&bridge](const J& a, Call& call) {
                         int changed = 0;
                         J skipped = J::array();
                         for (const auto& idv : a["presets"]) {
                             const std::string id = idv.get<std::string>();
                             if (id.rfind("user:", 0) != 0) {
                                 skipped.push_back({{"preset", id}, {"reason", "factory presets are read-only"}});
                                 continue;
                             }
                             const fs::path file = bridge.presetPath(id);
                             std::vector<std::uint8_t> bytes;
                             std::string error;
                             if (file.empty() || !zed::readFile(utf8(file), bytes, error)) {
                                 skipped.push_back({{"preset", id}, {"reason", "not found"}});
                                 continue;
                             }
                             auto loaded = zed::loadPatch(std::string(bytes.begin(), bytes.end()));
                             if (!loaded.ok()) {
                                 skipped.push_back({{"preset", id}, {"reason", "cannot be read"}});
                                 continue;
                             }
                             auto& m = loaded.patch.meta;
                             const auto before = m;
                             if (a.contains("set_tags"))
                                 m.tags = a["set_tags"].get<std::vector<std::string>>();
                             for (const auto& t : a.value("add_tags", J::array()))
                                 if (std::find(m.tags.begin(), m.tags.end(), t.get<std::string>()) == m.tags.end())
                                     m.tags.push_back(t.get<std::string>());
                             for (const auto& t : a.value("remove_tags", J::array()))
                                 m.tags.erase(std::remove(m.tags.begin(), m.tags.end(), t.get<std::string>()), m.tags.end());
                             if (a.contains("genres")) m.genres = a["genres"].get<std::vector<std::string>>();
                             if (a.contains("type")) m.type = argString(a, "type");
                             if (a.contains("pack")) m.pack = argString(a, "pack");
                             if (a.contains("author")) m.author = argString(a, "author");
                             if (a.contains("rating")) m.rating = argInt(a, "rating", 0);
                             if (m == before)
                                 continue;
                             const fs::path checked = bridge.resolveWrite(utf8(file), {".zpreset"}, "/presets", bridge.presetsDir(false));
                             std::ofstream f(checked, std::ios::binary);
                             f << zed::savePatch(loaded.patch);
                             if (!f) {
                                 skipped.push_back({{"preset", id}, {"reason", "could not be written"}});
                                 continue;
                             }
                             ++changed;
                         }
                         if (changed > 0)
                             bridge.report(call, "changed the details of " + std::to_string(changed) + (changed == 1 ? " preset" : " presets"));
                         ToolOutput out;
                         out.structured = {{"changed", changed}, {"skipped", skipped}};
                         out.summary = std::to_string(changed) + " preset(s) changed, " + std::to_string(skipped.size()) + " skipped.";
                         return out;
                     }});
}

} // namespace zmcp
