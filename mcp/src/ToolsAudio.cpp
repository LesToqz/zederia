#include "Tools.h"

#include "zederia/analysis/Analysis.h"
#include "zederia/midi/MidiFile.h"
#include "zederia/music/Theory.h"
#include "zederia/params/ValueText.h"
#include "zederia/util/Compress.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>

namespace zmcp {

using zed::Patch;

namespace {

std::vector<std::uint8_t> readBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void writeBytes(const fs::path& p, const std::vector<std::uint8_t>& bytes) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f)
        fail("", "the file " + utf8(p) + " could not be written", "check the folder is writable and the disk has room");
}

zed::WavFormat wavFormat(const std::string& s) {
    return s == "pcm16" ? zed::WavFormat::Pcm16 : s == "float32" ? zed::WavFormat::Float32 : zed::WavFormat::Pcm24;
}

// A file stem that does not exist yet in `dir` ("name", "name_2", "name_3" ...).
fs::path freshPath(const fs::path& dir, const std::string& stem, const std::string& ext, bool overwrite) {
    fs::path p = dir / pathFromUtf8(stem + ext);
    std::error_code ec;
    for (int n = 2; !overwrite && fs::exists(p, ec) && n < 10000; ++n)
        p = dir / pathFromUtf8(stem + "_" + std::to_string(n) + ext);
    return p;
}

struct Rendered {
    std::vector<float> left, right, tailLeft, tailRight;
    double sampleRate = 48000.0;
    double noteOffSeconds = -1.0;
    double loopSeconds = 0.0;
};

struct Job {
    Patch patch;
    std::vector<zed::NoteSpec> notes; // seconds
    double bpm = 120.0;
    double sampleRate = 48000.0;
    double loopSeconds = 0.0;   // 0: not a loop
    double tailSeconds = 8.0;   // most rendered after the last note (or the loop)
    std::string tail = "natural"; // natural, cut, separate, fold
    bool offline = true;
    std::uint64_t seed = 1;
};

void trimSilence(std::vector<float>& l, std::vector<float>& r, double sr, std::size_t keepAtLeast) {
    const float floor = 3.2e-5f; // -90 dBFS
    std::size_t end = std::min(l.size(), r.size());
    while (end > keepAtLeast && std::abs(l[end - 1]) < floor && std::abs(r[end - 1]) < floor)
        --end;
    end = std::min(std::min(l.size(), r.size()), end + static_cast<std::size_t>(sr * 0.02));
    l.resize(std::max(end, keepAtLeast));
    r.resize(std::max(end, keepAtLeast));
}

Rendered runRender(Bridge& bridge, const Job& job, Call& call, double from, double to, std::vector<std::string>& problems) {
    auto settings = bridge.renderSettings(job.patch, problems);
    settings.sampleRate = job.sampleRate;
    settings.bpm = job.bpm;
    settings.seed = job.seed;
    settings.offlineQuality = job.offline;
    settings.tailSeconds = std::clamp(job.tailSeconds, 0.0, kMaxTailSeconds);
    settings.minSeconds = job.loopSeconds > 0.0 ? job.loopSeconds + (job.tail == "cut" ? 0.0 : settings.tailSeconds) : 0.0;
    settings.progress = [&](double f) {
        call.progress(from + (to - from) * f, "rendering");
        return !call.cancelled();
    };
    const auto events = zed::sequenceFromNotes(job.notes, job.sampleRate);
    auto result = zed::renderOffline(job.patch, events, settings);
    if (result.cancelled)
        throw Cancelled{};
    Rendered out;
    out.sampleRate = result.sampleRate;
    for (const auto& n : job.notes)
        out.noteOffSeconds = std::max(out.noteOffSeconds, n.start + n.duration);
    out.loopSeconds = job.loopSeconds;
    if (job.loopSeconds <= 0.0) {
        trimSilence(result.left, result.right, result.sampleRate,
                    static_cast<std::size_t>(std::max(0.0, out.noteOffSeconds) * result.sampleRate));
        out.left = std::move(result.left);
        out.right = std::move(result.right);
        return out;
    }
    const auto loopLen = static_cast<std::size_t>(std::llround(job.loopSeconds * result.sampleRate));
    result.left.resize(std::max(result.left.size(), loopLen));
    result.right.resize(std::max(result.right.size(), loopLen));
    out.left.assign(result.left.begin(), result.left.begin() + static_cast<std::ptrdiff_t>(loopLen));
    out.right.assign(result.right.begin(), result.right.begin() + static_cast<std::ptrdiff_t>(loopLen));
    std::vector<float> tl(result.left.begin() + static_cast<std::ptrdiff_t>(loopLen), result.left.end());
    std::vector<float> tr(result.right.begin() + static_cast<std::ptrdiff_t>(loopLen), result.right.end());
    trimSilence(tl, tr, result.sampleRate, 0);
    if (job.tail == "fold") { // the tail wraps round onto the loop's start, so the loop repeats seamlessly
        for (std::size_t i = 0; i < tl.size(); ++i) {
            out.left[i % loopLen] += tl[i];
            out.right[i % loopLen] += tr[i];
        }
    } else if (job.tail == "separate") {
        out.tailLeft = std::move(tl);
        out.tailRight = std::move(tr);
    }
    return out;
}

// Notes from the arguments (beats or seconds) as seconds.
std::vector<zed::NoteSpec> notesFromArgs(const J& list, bool beats, double bpm, const std::string& path) {
    std::vector<zed::NoteSpec> notes;
    const double k = beats ? 60.0 / bpm : 1.0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const J& n = list[i];
        zed::NoteSpec s;
        s.note = noteArg(n["note"], path + "/" + std::to_string(i) + "/note");
        s.start = argNumber(n, "start", 0.0) * k;
        s.duration = argNumber(n, "length", 0.5) * k;
        s.velocity = velocityArg(n.value("velocity", J()), 0.8);
        notes.push_back(s);
    }
    return notes;
}

std::vector<zed::NoteSpec> notesFromClip(const zed::Clip& c, double bpm) {
    std::vector<zed::NoteSpec> notes;
    const double k = 60.0 / bpm, end = c.lengthBeats();
    for (const auto& n : c.notes) {
        if (n.start >= end)
            continue;
        notes.push_back({n.key, n.start * k, (std::min(n.start + n.length, end) - n.start) * k, n.velocity});
    }
    return notes;
}

struct Written {
    J info = J::object();
    std::vector<J> content;
};

// Writes the audio (and tail, MIDI, spectrogram, sidecar) and analyses it.
Written writeRender(const Rendered& r, const fs::path& dir, const std::string& stem, const J& a, const zed::Clip* clip,
                    const std::string& targetName, const std::vector<std::string>& problems) {
    Written w;
    const bool overwrite = argBool(a, "overwrite", false);
    const fs::path wav = freshPath(dir, stem, ".wav", overwrite);
    const std::string finalStem = utf8(wav.stem());
    const auto fmt = wavFormat(argString(a, "format", "pcm24"));
    writeBytes(wav, zed::encodeWav(r.left, r.right, r.sampleRate, fmt));
    w.info["file"] = utf8(wav);
    w.info["uri"] = "zederia://renders/" + finalStem;
    w.info["duration_s"] = std::round(static_cast<double>(r.left.size()) / r.sampleRate * 1000.0) / 1000.0;
    w.info["sample_rate"] = r.sampleRate;
    if (!r.tailLeft.empty()) {
        const fs::path tail = dir / pathFromUtf8(finalStem + "_tail.wav");
        writeBytes(tail, zed::encodeWav(r.tailLeft, r.tailRight, r.sampleRate, fmt));
        w.info["tail_file"] = utf8(tail);
    }
    if (clip && argBool(a, "midi", true)) {
        const fs::path mid = dir / pathFromUtf8(finalStem + ".mid");
        writeBytes(mid, zed::midi::clipToFile(*clip));
        w.info["midi_file"] = utf8(mid);
    }
    zed::analysis::Audio audio{r.left, r.right, r.sampleRate};
    zed::analysis::Options opts;
    opts.noteOffSeconds = r.loopSeconds > 0.0 ? -1.0 : r.noteOffSeconds;
    const auto d = zed::analysis::analyze(audio, opts);
    w.info["analysis"] = zed::analysis::toJson(d);
    if (argBool(a, "spectrogram", false)) {
        const auto png = zed::analysis::melSpectrogramPng(audio);
        const fs::path img = dir / pathFromUtf8(finalStem + "_mel.png");
        writeBytes(img, png);
        w.info["spectrogram_file"] = utf8(img);
        w.content.push_back({{"type", "image"}, {"mimeType", "image/png"}, {"data", zed::base64Encode(png)}});
    }
    if (!problems.empty())
        w.info["problems"] = problems;
    w.content.insert(w.content.begin(), J{{"type", "resource_link"},
                                          {"uri", "zederia://renders/" + finalStem},
                                          {"name", finalStem + ".wav"},
                                          {"mimeType", "audio/wav"},
                                          {"description", "rendered audio at " + utf8(wav)}});
    J sidecar = w.info;
    sidecar["format"] = "zederia.render";
    sidecar["target"] = targetName;
    sidecar["created"] = static_cast<std::int64_t>(std::time(nullptr));
    std::ofstream(dir / pathFromUtf8(finalStem + ".json"), std::ios::binary) << sidecar.dump(1);
    return w;
}

std::string describeAnalysis(const J& an) {
    const J& lvl = an["level"];
    const J& tm = an["time"];
    const J& p = an["pitch"];
    std::string s = "peak " + lvl["peak_dbfs"].dump() + " dBFS, loudness " + lvl["loudness_lufs"].dump() + " LUFS, attack " + tm["attack_ms"].dump() +
                    " ms, " + tm["decay_shape"].get<std::string>() + ", centroid " + an["spectrum"]["centroid_hz"].dump() + " Hz, width " +
                    an["stereo"]["width"].dump();
    if (p.value("pitched", false))
        s += ", pitch " + p.value("note", "") + " " + p["cents_off"].dump() + " c";
    std::string tags;
    for (const auto& t : an["tags"])
        tags += (tags.empty() ? "" : ", ") + t.get<std::string>();
    return s + "; tags: " + tags;
}

void checkLength(double seconds, const std::string& path) {
    if (seconds > kMaxRenderSeconds)
        fail(path, "the render would last " + std::to_string(static_cast<int>(seconds)) + " s; at most " + std::to_string(static_cast<int>(kMaxRenderSeconds)) +
                       " s are allowed per call",
             "render shorter phrases, or split the material over several calls");
}

const char* kRenderSchema = R"json({"type":"object","additionalProperties":false,"required":["target"],"properties":{
    "target":{"type":"string","description":"a target handle; a live instance renders a copy of its current state here, off its audio thread"},
    "notes":{"$ref":"#/$defs/notes"},
    "clip":{"type":"string","description":"a clip id from write_clip: renders a loop-ready file and its .mid"},
    "units":{"enum":["beats","seconds"],"description":"for notes (default beats)"},
    "tempo":{"type":"number","minimum":20,"maximum":400,"description":"bpm (default: the clip's, else 120)"},
    "key":{"type":"string","description":"clip only: transpose the clip into this key, for example \"G minor\""},
    "tail":{"enum":["natural","cut","separate","fold"],"description":"natural (notes, default): render until silent; for clips cut, separate (a _tail.wav) or fold (default: the tail wraps onto the loop start)"},
    "max_tail_seconds":{"type":"number","minimum":0,"maximum":30},
    "quality":{"enum":["offline","live"]},
    "sample_rate":{"enum":[44100,48000,88200,96000]},
    "format":{"enum":["pcm16","pcm24","float32"]},
    "name":{"type":"string","maxLength":120,"description":"file name without extension (default: from the patch and clip)"},
    "folder":{"type":"string","maxLength":200,"description":"sub-folder of the Renders folder"},
    "overwrite":{"type":"boolean"},
    "spectrogram":{"type":"boolean","description":"also return a mel spectrogram image"},
    "midi":{"type":"boolean","description":"clip: write the .mid beside the audio (default true)"},
    "seed":{"type":"integer","minimum":1}}})json";

} // namespace

void addAudioTools(std::vector<ToolDef>& tools, Bridge& bridge) {
    // --- render_audio ---
    {
        J in = schema(kRenderSchema);
        in["$defs"] = {{"notes", notesSchema()}};
        tools.push_back({"render_audio", "Render and analyze",
                         "Renders notes or a clip offline with the target's patch and returns the file path, a resource link and the "
                         "analysis (level, loudness, attack, decay, tail, pitch, brightness, noisiness, band balance, stereo width, movement "
                         "and plain tags). A clip renders as a loop of its bars with a matching .mid; tail cut, separate or fold. Reports "
                         "progress and honours cancellation. Files go to the user's Renders folder.",
                         in,
                         schema(R"json({"type":"object","required":["file","uri","duration_s","sample_rate","analysis","tail"],"properties":{
                            "file":{"type":"string"},"uri":{"type":"string"},"duration_s":{"type":"number"},"sample_rate":{"type":"number"},
                            "tail":{"type":"string"},"tail_file":{"type":"string"},"midi_file":{"type":"string"},"spectrogram_file":{"type":"string"},
                            "loop":{"type":"object","required":["bars","beats","tempo","seconds"]},"analysis":{"type":"object","required":["level","time","pitch","spectrum","motion","stereo","tags"]},
                            "problems":{"type":"array"},"clip_issues":{"type":"array"}}})json"),
                         false, false, false, [&bridge](const J& a, Call& call) {
                             const std::string t = argString(a, "target");
                             bridge.checkTarget(t);
                             Job job;
                             job.patch = bridge.patch(t, call);
                             job.sampleRate = argNumber(a, "sample_rate", 48000.0);
                             job.offline = argString(a, "quality", "offline") == "offline";
                             job.seed = static_cast<std::uint64_t>(argInt(a, "seed", 1));
                             job.tailSeconds = argNumber(a, "max_tail_seconds", 8.0);
                             zed::Clip clip;
                             const bool isClip = a.contains("clip");
                             if (isClip == a.contains("notes"))
                                 fail("", "give either notes or a clip", "pass notes [{note, start, length, velocity}] or a clip id from write_clip");
                             if (isClip) {
                                 if (!bridge.loadClip(argString(a, "clip"), clip))
                                     fail("/clip", "no clip \"" + argString(a, "clip") + "\"", "write one with write_clip and pass its clip_id");
                                 if (a.contains("key")) {
                                     zed::music::Key from, to;
                                     if (!zed::music::parseKey(argString(a, "key"), to))
                                         fail("/key", "cannot read the key \"" + argString(a, "key") + "\"", "write it like \"F minor\" or \"Bb major\"");
                                     if (!zed::music::parseKey(clip.key, from))
                                         fail("/key", "the clip has no key to transpose from", "set the clip's key with write_clip");
                                     clip = zed::music::transposeClip(clip, zed::music::transposeBetween(from.tonic, to.tonic));
                                     clip.key = to.name();
                                 }
                                 job.bpm = argNumber(a, "tempo", clip.tempo);
                                 clip.tempo = job.bpm;
                                 job.notes = notesFromClip(clip, job.bpm);
                                 job.loopSeconds = clip.lengthBeats() * 60.0 / job.bpm;
                                 job.tail = argString(a, "tail", "fold");
                                 if (job.tail == "natural")
                                     fail("/tail", "a clip renders as a loop", "use tail cut, separate or fold");
                             } else {
                                 job.bpm = argNumber(a, "tempo", 120.0);
                                 job.notes = notesFromArgs(a["notes"], argString(a, "units", "beats") == "beats", job.bpm, "/notes");
                                 job.tail = argString(a, "tail", "natural");
                                 if (job.tail != "natural")
                                     fail("/tail", "tail options cut, separate and fold are for clips", "use natural for notes, or write a clip");
                                 if (job.notes.empty())
                                     fail("/notes", "there are no notes to render", "add at least one note");
                             }
                             double end = job.loopSeconds;
                             for (const auto& n : job.notes)
                                 end = std::max(end, n.start + n.duration);
                             checkLength(end, isClip ? "/clip" : "/notes");
                             std::vector<std::string> problems;
                             call.progress(0.0, "rendering");
                             const Rendered r = runRender(bridge, job, call, 0.0, 0.95, problems);
                             std::string folder = argString(a, "folder", "");
                             const fs::path dir = folder.empty() ? bridge.userFolder("Renders")
                                                                 : bridge.resolveWrite(folder + "/x.wav", {".wav"}, "/folder", bridge.userFolder("Renders")).parent_path();
                             std::string stem = argString(a, "name", "");
                             if (stem.empty()) {
                                 if (isClip) {
                                     zed::music::Key k;
                                     const std::string keyShort = zed::music::parseKey(clip.key, k) ? k.shortName() : "";
                                     stem = zed::music::loopFileName(clip.name, displayName(job.patch), keyShort, job.bpm, "wav");
                                     stem = stem.substr(0, stem.size() - 4);
                                 } else {
                                     stem = "ZED_render_" + zed::music::compactName(displayName(job.patch));
                                 }
                             }
                             if (stem.find_first_of("/\\:*?\"<>|") != std::string::npos || stem.find("..") != std::string::npos)
                                 fail("/name", "the name cannot contain / \\ : * ? \" < > | or ..", "use letters, digits, - and _");
                             Written w = writeRender(r, dir, stem, a, isClip ? &clip : nullptr, t, problems);
                             w.info["tail"] = job.tail;
                             if (isClip) {
                                 w.info["loop"] = {{"bars", clip.bars}, {"beats", clip.lengthBeats()}, {"tempo", job.bpm}, {"seconds", job.loopSeconds}};
                                 J issues = J::array();
                                 zed::music::Key k;
                                 zed::music::parseKey(clip.key, k);
                                 for (const auto& is : zed::music::validateClip(clip, k))
                                     if (!is.error)
                                         issues.push_back({{"code", is.code}, {"message", is.message}});
                                 if (!issues.empty())
                                     w.info["clip_issues"] = issues;
                             }
                             call.progress(1.0, "done");
                             ToolOutput out;
                             out.structured = w.info;
                             out.content = w.content;
                             out.summary = "Rendered " + w.info["duration_s"].dump() + " s to " + w.info["file"].get<std::string>() + ". " +
                                           describeAnalysis(w.info["analysis"]) + ".";
                             return out;
                         }});
    }

    // --- analyze_audio ---
    tools.push_back({"analyze_audio", "Analyze an audio file",
                     "Measures any WAV file inside an allowed folder (Zederia's folders, or folders the user allowed): level, loudness, "
                     "attack, decay, tail, pitch and stability, brightness, noisiness, band balance, stereo width, movement and tags.",
                     schema(R"json({"type":"object","additionalProperties":false,"required":["path"],"properties":{
                        "path":{"type":"string","description":"a .wav file; relative paths start in the user's Zederia folder"},
                        "note_off_s":{"type":"number","minimum":0,"description":"when the note was let go, for the tail measurement"},
                        "spectrogram":{"type":"boolean"}}})json"),
                     schema(R"json({"type":"object","required":["path","analysis"],"properties":{"path":{"type":"string"},
                        "analysis":{"type":"object","required":["level","time","pitch","spectrum","motion","stereo","tags"]}}})json"),
                     true, false, true, [&bridge](const J& a, Call&) {
                         const fs::path file = bridge.resolveRead(argString(a, "path"), {".wav"}, "/path");
                         std::vector<float> l, r;
                         double sr = 48000.0;
                         std::string error;
                         if (!zed::analysis::loadAudioFile(readBytes(file), l, r, sr, error))
                             fail("/path", "the file cannot be read: " + error, "use a PCM or float WAV file");
                         if (static_cast<double>(l.size()) / sr > 600.0)
                             fail("/path", "the file is longer than 10 minutes", "analyze a shorter excerpt");
                         zed::analysis::Audio audio{l, r, sr};
                         zed::analysis::Options o;
                         o.noteOffSeconds = argNumber(a, "note_off_s", -1.0);
                         ToolOutput out;
                         out.structured = {{"path", utf8(file)}, {"analysis", zed::analysis::toJson(zed::analysis::analyze(audio, o))}};
                         if (argBool(a, "spectrogram", false))
                             out.content.push_back({{"type", "image"}, {"mimeType", "image/png"}, {"data", zed::base64Encode(zed::analysis::melSpectrogramPng(audio))}});
                         out.summary = describeAnalysis(out.structured["analysis"]) + ".";
                         return out;
                     }});

    // --- compare_audio ---
    tools.push_back({"compare_audio", "Compare two audio files",
                     "Scores how close file a is to file b (a reference): spectral difference (40 mel bands, loudness aligned), envelope "
                     "difference, loudness, peak, brightness, attack, width and pitch differences, one distance to minimise and a "
                     "similarity from 0 to 1. Use it after each change when matching a reference.",
                     schema(R"json({"type":"object","additionalProperties":false,"required":["a","b"],"properties":{
                        "a":{"type":"string","description":"the file being judged, usually a render"},
                        "b":{"type":"string","description":"the reference"}}})json"),
                     schema(R"json({"type":"object","required":["a","b","comparison"],"properties":{"a":{"type":"string"},"b":{"type":"string"},
                        "comparison":{"type":"object","required":["spectral_db","envelope_db","loudness_diff_lu","distance","similarity"]}}})json"),
                     true, false, true, [&bridge](const J& a, Call&) {
                         const fs::path fa = bridge.resolveRead(argString(a, "a"), {".wav"}, "/a");
                         const fs::path fb = bridge.resolveRead(argString(a, "b"), {".wav"}, "/b");
                         std::vector<float> la, ra, lb, rb;
                         double sa = 48000, sb = 48000;
                         std::string error;
                         if (!zed::analysis::loadAudioFile(readBytes(fa), la, ra, sa, error))
                             fail("/a", "cannot read: " + error, "use a WAV file");
                         if (!zed::analysis::loadAudioFile(readBytes(fb), lb, rb, sb, error))
                             fail("/b", "cannot read: " + error, "use a WAV file");
                         const auto c = zed::analysis::compare({la, ra, sa}, {lb, rb, sb});
                         ToolOutput out;
                         out.structured = {{"a", utf8(fa)}, {"b", utf8(fb)}, {"comparison", zed::analysis::toJson(c)}};
                         const J& j = out.structured["comparison"];
                         out.summary = "Distance " + j["distance"].dump() + " (similarity " + j["similarity"].dump() + "): spectrum " + j["spectral_db"].dump() +
                                       " dB, envelope " + j["envelope_db"].dump() + " dB, loudness " + j["loudness_diff_lu"].dump() + " LU, brightness " +
                                       j["centroid_diff_oct"].dump() + " oct.";
                         return out;
                     }});

    // --- audition ---
    {
        J in = schema(R"json({"type":"object","additionalProperties":false,"required":["target"],"properties":{
            "target":{"type":"string","description":"a \"live:...\" instance"},
            "notes":{"$ref":"#/$defs/notes"},
            "clip":{"type":"string"},
            "units":{"enum":["beats","seconds"]},
            "tempo":{"type":"number","minimum":20,"maximum":400}}})json");
        in["$defs"] = {{"notes", notesSchema()}};
        tools.push_back({"audition", "Play through a live instance",
                         "Plays notes or a clip through a live instance, so the user hears them in their host (level-capped; the user can "
                         "switch it off on the AGENT page). At most 30 seconds.",
                         in,
                         schema(R"json({"type":"object","required":["target","played","duration_s"],"properties":{"target":{"type":"string"},"played":{"type":"integer"},"duration_s":{"type":"number"}}})json"),
                         false, false, false, [&bridge](const J& a, Call& call) {
                             const std::string t = argString(a, "target");
                             if (!Bridge::isLive(t))
                                 fail("/target", "audition plays through a live instance", "for a headless session use render_audio and give the user the file");
                             bridge.checkTarget(t);
                             const auto perms = bridge.permissions();
                             if (!perms.audition)
                                 fail("/target", "auditioning through the speakers is switched off", "the user can allow it on the AGENT page (Permissions)");
                             std::vector<zed::NoteSpec> notes;
                             double bpm = argNumber(a, "tempo", 120.0);
                             if (a.contains("clip")) {
                                 zed::Clip c;
                                 if (!bridge.loadClip(argString(a, "clip"), c))
                                     fail("/clip", "no clip \"" + argString(a, "clip") + "\"", "use a clip id from write_clip");
                                 bpm = argNumber(a, "tempo", c.tempo);
                                 notes = notesFromClip(c, bpm);
                             } else if (a.contains("notes")) {
                                 notes = notesFromArgs(a["notes"], argString(a, "units", "beats") == "beats", bpm, "/notes");
                             } else {
                                 fail("", "give notes or a clip", "pass notes [{note, start, length}] or a clip id");
                             }
                             double end = 0.0;
                             J list = J::array();
                             for (const auto& n : notes) {
                                 end = std::max(end, n.start + n.duration);
                                 list.push_back({{"note", n.note}, {"start", n.start}, {"duration", n.duration}, {"velocity", n.velocity}});
                             }
                             if (end > 30.0)
                                 fail("/notes", "an audition may last at most 30 seconds", "play a shorter phrase");
                             bridge.liveRequest(t, "audition", {{"notes", list}, {"cap_db", perms.auditionCapDb}}, call);
                             ToolOutput out;
                             out.structured = {{"target", t}, {"played", notes.size()}, {"duration_s", end}};
                             out.summary = "Playing " + std::to_string(notes.size()) + " note(s) through " + t + ".";
                             return out;
                         }});
    }

    // --- export_pack ---
    tools.push_back({"export_pack", "Export a sample pack",
                     "Renders presets x clips x keys into a named folder under the user's Packs folder, with names such as "
                     "ZED_chords_GlassPluck_Fmin_124bpm.wav (and the .mid of each loop), plus one-shots (ZED_oneshot_<Preset>_<Note>.wav) "
                     "and multi-sampled notes (ZED_<Preset>_<Note>_v<velocity>.wav). Presets are preset ids or target handles. At most "
                     "256 files per call. Reports progress and honours cancellation.",
                     schema(R"json({"type":"object","additionalProperties":false,"required":["name","presets"],"properties":{
                        "name":{"type":"string","minLength":1,"maxLength":80,"description":"the pack's folder name"},
                        "presets":{"type":"array","minItems":1,"maxItems":64,"items":{"type":"string"},"description":"preset ids or target handles"},
                        "clips":{"type":"array","maxItems":32,"items":{"type":"string"}},
                        "keys":{"type":"array","maxItems":12,"items":{"type":"string"},"description":"transpose each clip into these keys"},
                        "tempo":{"type":"number","minimum":20,"maximum":400},
                        "tail":{"enum":["cut","separate","fold"]},
                        "one_shots":{"type":"object","additionalProperties":false,"properties":{"notes":{"type":"array","minItems":1,"maxItems":24},
                            "length_s":{"type":"number","exclusiveMinimum":0,"maximum":20},"velocity":{"type":"number","minimum":0,"maximum":127}}},
                        "multi_samples":{"type":"object","additionalProperties":false,"properties":{"notes":{"type":"array","minItems":1,"maxItems":48},
                            "velocities":{"type":"array","items":{"type":"number","minimum":1,"maximum":127}},"length_s":{"type":"number","exclusiveMinimum":0,"maximum":20}}},
                        "format":{"enum":["pcm16","pcm24","float32"]},
                        "sample_rate":{"enum":[44100,48000,88200,96000]},
                        "midi":{"type":"boolean"},
                        "overwrite":{"type":"boolean"}}})json"),
                     schema(R"json({"type":"object","required":["folder","files","count"],"properties":{"folder":{"type":"string"},"count":{"type":"integer"},
                        "files":{"type":"array","items":{"type":"object","required":["file","kind","preset"]}}}})json"),
                     false, false, false, [&bridge](const J& a, Call& call) {
                         const std::string name = argString(a, "name");
                         if (name.find_first_of("/\\:*?\"<>|") != std::string::npos || name.find("..") != std::string::npos)
                             fail("/name", "the pack name cannot contain / \\ : * ? \" < > | or ..", "use letters, digits, spaces, - and _");
                         const fs::path dir = bridge.resolveWrite(name + "/pack.wav", {".wav"}, "/name", bridge.userFolder("Packs")).parent_path();
                         // Gather the presets.
                         std::vector<Patch> patches;
                         std::vector<std::string> sources;
                         const J presets = a["presets"];
                         for (std::size_t i = 0; i < presets.size(); ++i) {
                             const std::string id = presets[i].get<std::string>();
                             if (Bridge::isSession(id) || Bridge::isLive(id)) {
                                 bridge.checkTarget(id, "/presets/" + std::to_string(i));
                                 patches.push_back(bridge.patch(id, call));
                             } else {
                                 const fs::path file = bridge.presetPath(id);
                                 std::vector<std::uint8_t> bytes;
                                 std::string error;
                                 if (file.empty() || !zed::readFile(utf8(file), bytes, error))
                                     fail("/presets/" + std::to_string(i), "no preset \"" + id + "\"", "use ids from list_presets or target handles");
                                 auto loaded = zed::loadPatch(std::string(bytes.begin(), bytes.end()));
                                 if (!loaded.ok())
                                     fail("/presets/" + std::to_string(i), "the preset cannot be read", "leave it out");
                                 patches.push_back(loaded.patch);
                             }
                             sources.push_back(id);
                         }
                         std::vector<zed::Clip> clips;
                         const J clipIds = a.value("clips", J::array());
                         for (std::size_t i = 0; i < clipIds.size(); ++i) {
                             zed::Clip c;
                             if (!bridge.loadClip(clipIds[i].get<std::string>(), c))
                                 fail("/clips/" + std::to_string(i), "no clip " + clipIds[i].dump(), "use clip ids from write_clip");
                             clips.push_back(c);
                         }
                         std::vector<zed::music::Key> keys;
                         const J keyList = a.value("keys", J::array());
                         for (std::size_t i = 0; i < keyList.size(); ++i) {
                             zed::music::Key k;
                             if (!zed::music::parseKey(keyList[i].get<std::string>(), k))
                                 fail("/keys/" + std::to_string(i), "cannot read the key " + keyList[i].dump(), "write keys like \"F minor\"");
                             keys.push_back(k);
                         }
                         if (!keys.empty())
                             for (std::size_t i = 0; i < clips.size(); ++i) {
                                 zed::music::Key k;
                                 if (!zed::music::parseKey(clips[i].key, k))
                                     fail("/clips/" + std::to_string(i), "the clip has no key, so it cannot be transposed", "give the clip a key with write_clip");
                             }
                         const J os = a.value("one_shots", J::object()), ms = a.value("multi_samples", J::object());
                         const J osNotes = os.value("notes", J::array()), msNotes = ms.value("notes", J::array());
                         J velocities = ms.value("velocities", J::array({100}));
                         if (velocities.empty())
                             velocities = J::array({100});
                         const std::size_t perPreset = clips.size() * std::max<std::size_t>(1, keys.size()) + osNotes.size() + msNotes.size() * velocities.size();
                         const std::size_t total = perPreset * patches.size();
                         if (total == 0)
                             fail("", "nothing to render", "give clips, one_shots or multi_samples");
                         if (total > static_cast<std::size_t>(kMaxPackFiles))
                             fail("", "the pack would have " + std::to_string(total) + " files; at most " + std::to_string(kMaxPackFiles) + " per call",
                                  "export fewer presets, clips or keys per call");
                         double seconds = 0.0;
                         for (const auto& c : clips)
                             seconds += c.lengthBeats() * 60.0 / argNumber(a, "tempo", c.tempo) * std::max<std::size_t>(1, keys.size());
                         seconds += argNumber(os, "length_s", 2.0) * static_cast<double>(osNotes.size()) +
                                    argNumber(ms, "length_s", 3.0) * static_cast<double>(msNotes.size() * velocities.size());
                         if (seconds * static_cast<double>(patches.size()) > kMaxPackSeconds)
                             fail("", "the pack would hold more than 30 minutes of audio", "export it in several calls");
                         J files = J::array();
                         std::size_t done = 0;
                         J args = a; // per-file options for writeRender
                         args["spectrogram"] = false;
                         const auto step = [&](const Job& job, const std::string& stem, const zed::Clip* clip, const std::string& kind, J extra) {
                             call.checkCancelled();
                             std::vector<std::string> problems;
                             const double from = static_cast<double>(done) / static_cast<double>(total);
                             const Rendered r = runRender(bridge, job, call, from, from + 1.0 / static_cast<double>(total), problems);
                             Written w = writeRender(r, dir, stem, args, clip, sources[0], problems);
                             extra["file"] = w.info["file"];
                             extra["kind"] = kind;
                             if (w.info.contains("midi_file"))
                                 extra["midi_file"] = w.info["midi_file"];
                             if (w.info.contains("tail_file"))
                                 extra["tail_file"] = w.info["tail_file"];
                             extra["loudness_lufs"] = w.info["analysis"]["level"]["loudness_lufs"];
                             extra["peak_dbfs"] = w.info["analysis"]["level"]["peak_dbfs"];
                             files.push_back(extra);
                             ++done;
                             call.progress(static_cast<double>(done) / static_cast<double>(total), utf8(fs::path(pathFromUtf8(stem)).filename()));
                         };
                         for (std::size_t p = 0; p < patches.size(); ++p) {
                             const std::string pname = displayName(patches[p]);
                             for (const auto& c : clips) {
                                 const std::vector<zed::music::Key> variants = keys.empty() ? std::vector<zed::music::Key>{zed::music::Key{}} : keys;
                                 for (const auto& k : variants) {
                                     zed::Clip use = c;
                                     zed::music::Key ck;
                                     zed::music::parseKey(c.key, ck);
                                     if (k.valid()) {
                                         use = zed::music::transposeClip(c, zed::music::transposeBetween(ck.tonic, k.tonic));
                                         use.key = k.name();
                                         ck = k;
                                     }
                                     Job job;
                                     job.patch = patches[p];
                                     job.bpm = argNumber(a, "tempo", c.tempo);
                                     use.tempo = job.bpm;
                                     job.sampleRate = argNumber(a, "sample_rate", 48000.0);
                                     job.notes = notesFromClip(use, job.bpm);
                                     job.loopSeconds = use.lengthBeats() * 60.0 / job.bpm;
                                     job.tail = argString(a, "tail", "fold");
                                     std::string stem = zed::music::loopFileName(use.name, pname, ck.valid() ? ck.shortName() : "", job.bpm, "wav");
                                     stem = stem.substr(0, stem.size() - 4);
                                     step(job, stem, &use, "loop", {{"preset", sources[p]}, {"clip", use.name}, {"key", use.key}});
                                 }
                             }
                             for (std::size_t i = 0; i < osNotes.size(); ++i) {
                                 const int note = noteArg(osNotes[i], "/one_shots/notes/" + std::to_string(i));
                                 Job job;
                                 job.patch = patches[p];
                                 job.sampleRate = argNumber(a, "sample_rate", 48000.0);
                                 job.notes = {{note, 0.0, argNumber(os, "length_s", 2.0), velocityArg(os.value("velocity", J(100)), 0.8)}};
                                 std::string stem = zed::music::oneShotFileName(pname, note, "wav");
                                 step(job, stem.substr(0, stem.size() - 4), nullptr, "one_shot", {{"preset", sources[p]}, {"note", zed::noteName(note)}});
                             }
                             for (std::size_t i = 0; i < msNotes.size(); ++i) {
                                 const int note = noteArg(msNotes[i], "/multi_samples/notes/" + std::to_string(i));
                                 for (const auto& v : velocities) {
                                     const int vel = std::clamp(static_cast<int>(std::lround(v.get<double>())), 1, 127);
                                     Job job;
                                     job.patch = patches[p];
                                     job.sampleRate = argNumber(a, "sample_rate", 48000.0);
                                     job.notes = {{note, 0.0, argNumber(ms, "length_s", 3.0), vel / 127.0}};
                                     std::string stem = zed::music::multiSampleFileName(pname, note, vel, "wav");
                                     step(job, stem.substr(0, stem.size() - 4), nullptr, "multi_sample",
                                          {{"preset", sources[p]}, {"note", zed::noteName(note)}, {"velocity", vel}});
                                 }
                             }
                         }
                         ToolOutput out;
                         out.structured = {{"folder", utf8(dir)}, {"files", files}, {"count", files.size()}};
                         out.summary = "Exported " + std::to_string(files.size()) + " file(s) to " + utf8(dir) + ".";
                         out.content.push_back({{"type", "resource_link"}, {"uri", "file:///" + utf8(dir)}, {"name", name}, {"description", "the pack folder"}});
                         return out;
                     }});
}

} // namespace zmcp
