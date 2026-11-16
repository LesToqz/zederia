#include "zederia/midi/MidiFile.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace zed::midi {
namespace {

constexpr std::size_t kMaxFileBytes = 16u * 1024u * 1024u;

struct Reader {
    std::span<const std::uint8_t> data;
    std::size_t pos = 0;
    bool ok = true;
    bool has(std::size_t n) const { return ok && pos + n <= data.size(); }
    std::uint8_t byte() {
        if (!has(1)) {
            ok = false;
            return 0;
        }
        return data[pos++];
    }
    std::uint32_t be(int n) {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 8) | byte();
        return v;
    }
    std::uint32_t vlq() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const std::uint8_t b = byte();
            v = (v << 7) | (b & 0x7fu);
            if ((b & 0x80u) == 0)
                return v;
        }
        ok = false;
        return v;
    }
};

const char* const kSharpNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char* const kFlatNames[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};

void putVlq(std::vector<std::uint8_t>& out, std::uint32_t v) {
    std::uint8_t bytes[5];
    int n = 0;
    bytes[n++] = static_cast<std::uint8_t>(v & 0x7fu);
    while ((v >>= 7) != 0)
        bytes[n++] = static_cast<std::uint8_t>((v & 0x7fu) | 0x80u);
    while (n > 0)
        out.push_back(bytes[--n]);
}

void putBe(std::vector<std::uint8_t>& out, std::uint32_t v, int n) {
    for (int i = n - 1; i >= 0; --i)
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

} // namespace

bool parseKey(std::string_view text, int& tonic, bool& minor) {
    std::size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
        ++i;
    if (i >= text.size())
        return false;
    static const int letters[7] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
    if (letter < 'A' || letter > 'G')
        return false;
    int pc = letters[letter - 'A'];
    ++i;
    if (i < text.size() && text[i] == '#') {
        pc += 1;
        ++i;
    } else if (i < text.size() && text[i] == 'b') {
        pc -= 1;
        ++i;
    }
    while (i < text.size() && (std::isspace(static_cast<unsigned char>(text[i])) || text[i] == '-'))
        ++i;
    std::string mode;
    for (; i < text.size(); ++i)
        mode += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
    while (!mode.empty() && std::isspace(static_cast<unsigned char>(mode.back())))
        mode.pop_back();
    if (mode.empty() || mode == "major" || mode == "maj")
        minor = false;
    else if (mode == "minor" || mode == "min" || mode == "m")
        minor = true;
    else
        return false;
    tonic = ((pc % 12) + 12) % 12;
    return true;
}

int keySignature(int tonic, bool minor) {
    const int major = ((tonic + (minor ? 3 : 0)) % 12 + 12) % 12;
    int best = 0, bestAbs = 99;
    for (int sf = -7; sf <= 7; ++sf) {
        if (((7 * sf) % 12 + 12) % 12 == major && std::abs(sf) < bestAbs) {
            best = sf;
            bestAbs = std::abs(sf);
        }
    }
    return best;
}

std::string keyName(int sharps, bool minor) {
    sharps = std::clamp(sharps, -7, 7);
    const int major = ((7 * sharps) % 12 + 12) % 12;
    const int tonic = minor ? (major + 9) % 12 : major;
    const char* const* names = sharps < 0 ? kFlatNames : kSharpNames;
    return std::string(names[tonic]) + (minor ? " minor" : " major");
}

bool readNotes(std::span<const std::uint8_t> bytes, std::vector<ClipNote>& notes, FileInfo& info, std::string& error) {
    notes.clear();
    info = FileInfo{};
    if (bytes.size() > kMaxFileBytes) {
        error = "the file is too large to be a clip";
        return false;
    }
    Reader r{bytes};
    if (!r.has(14) || std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) != "MThd") {
        error = "this is not a Standard MIDI File (no MThd header)";
        return false;
    }
    r.pos = 4;
    const std::uint32_t headerLength = r.be(4);
    const std::uint32_t format = r.be(2);
    const std::uint32_t tracks = r.be(2);
    const std::uint32_t division = r.be(2);
    if (headerLength < 6 || format > 2) {
        error = "the MIDI header is damaged";
        return false;
    }
    if ((division & 0x8000u) != 0 || division == 0) {
        error = "files timed in SMPTE frames are not supported; save it with beats (ticks per quarter note)";
        return false;
    }
    info.ticksPerQuarter = static_cast<int>(division);
    r.pos = 8 + headerLength;
    bool tempoSeen = false, signatureSeen = false;
    struct Open {
        std::uint64_t tick;
        int channel, key;
        double velocity;
    };
    std::vector<Open> open;
    double endTicks = 0.0;
    for (std::uint32_t t = 0; t < tracks && r.has(8); ++t) {
        const bool isTrack = std::string_view(reinterpret_cast<const char*>(bytes.data() + r.pos), 4) == "MTrk";
        r.pos += 4;
        const std::uint32_t length = r.be(4);
        if (!r.ok || r.pos + length > bytes.size()) {
            error = "a track is cut short";
            return false;
        }
        const std::size_t end = r.pos + length;
        if (!isTrack) { // an unknown chunk: skip it
            r.pos = end;
            continue;
        }
        std::uint64_t tick = 0;
        std::uint8_t running = 0;
        open.clear();
        while (r.ok && r.pos < end) {
            tick += r.vlq();
            std::uint8_t status = r.byte();
            if (status < 0x80) { // running status: this byte is data
                if (running == 0) {
                    error = "a track uses running status before any status byte";
                    return false;
                }
                --r.pos;
                status = running;
            }
            if (status == 0xFF) {
                const std::uint8_t type = r.byte();
                const std::uint32_t len = r.vlq();
                if (!r.has(len)) {
                    r.ok = false;
                    break;
                }
                const std::uint8_t* d = bytes.data() + r.pos;
                if (type == 0x51 && len == 3 && !tempoSeen) {
                    const std::uint32_t micros = (static_cast<std::uint32_t>(d[0]) << 16) | (static_cast<std::uint32_t>(d[1]) << 8) | d[2];
                    if (micros > 0)
                        info.tempo = 60000000.0 / micros;
                    tempoSeen = true;
                } else if (type == 0x58 && len >= 2 && !signatureSeen) {
                    const int denominator = 1 << std::min<int>(d[1], 6);
                    info.beatsPerBar = std::clamp(static_cast<int>(std::lround(d[0] * 4.0 / denominator)), 1, 16);
                    signatureSeen = true;
                } else if (type == 0x59 && len == 2 && !info.hasKey) {
                    info.hasKey = true;
                    info.sharps = std::clamp(static_cast<int>(static_cast<std::int8_t>(d[0])), -7, 7);
                    info.minor = d[1] != 0;
                } else if (type == 0x03 && info.name.empty()) {
                    info.name.assign(reinterpret_cast<const char*>(d), std::min<std::uint32_t>(len, 128));
                } else if (type == 0x2F) {
                    endTicks = std::max(endTicks, static_cast<double>(tick));
                }
                r.pos += len;
                continue;
            }
            if (status == 0xF0 || status == 0xF7) {
                const std::uint32_t len = r.vlq();
                if (!r.has(len)) {
                    r.ok = false;
                    break;
                }
                r.pos += len;
                continue;
            }
            running = status;
            const int kind = status & 0xF0, channel = status & 0x0F;
            const int dataBytes = kind == 0xC0 || kind == 0xD0 ? 1 : 2;
            const std::uint8_t d1 = r.byte() & 0x7f;
            const std::uint8_t d2 = dataBytes == 2 ? static_cast<std::uint8_t>(r.byte() & 0x7f) : 0;
            if (kind == 0x90 && d2 > 0) {
                open.push_back({tick, channel, d1, d2 / 127.0});
            } else if (kind == 0x80 || (kind == 0x90 && d2 == 0)) {
                const auto it = std::find_if(open.begin(), open.end(), [&](const Open& o) { return o.channel == channel && o.key == d1; });
                if (it != open.end()) {
                    if (notes.size() < static_cast<std::size_t>(kMaxClipNotes))
                        notes.push_back({static_cast<double>(it->tick) / division,
                                         std::max(static_cast<double>(tick - it->tick), 1.0) / division, it->key, it->velocity});
                    open.erase(it);
                }
            }
        }
        if (!r.ok) {
            error = "a track is damaged";
            return false;
        }
        for (const Open& o : open) // notes never released end with their track
            if (notes.size() < static_cast<std::size_t>(kMaxClipNotes))
                notes.push_back({static_cast<double>(o.tick) / division,
                                 std::max(static_cast<double>(tick - o.tick), 1.0) / division, o.key, o.velocity});
        endTicks = std::max(endTicks, static_cast<double>(tick));
        r.pos = end;
    }
    info.endBeats = endTicks / division;
    std::stable_sort(notes.begin(), notes.end(), [](const ClipNote& a, const ClipNote& b) {
        return a.start != b.start ? a.start < b.start : a.key < b.key;
    });
    return true;
}

bool clipFromFile(std::span<const std::uint8_t> bytes, Clip& clip, std::string& error) {
    std::vector<ClipNote> notes;
    FileInfo info;
    if (!readNotes(bytes, notes, info, error))
        return false;
    if (notes.empty()) {
        error = "the file has no notes";
        return false;
    }
    Clip out;
    out.name = info.name;
    out.notes = std::move(notes);
    out.tempo = std::clamp(info.tempo, 20.0, 999.0);
    out.beatsPerBar = info.beatsPerBar;
    double last = 0.0;
    for (const ClipNote& n : out.notes)
        last = std::max(last, n.start + n.length);
    const double length = std::max(last, info.endBeats);
    out.bars = std::clamp(std::ceil(length / out.beatsPerBar - 1.0e-6), 1.0, 256.0);
    if (info.hasKey) {
        out.key = keyName(info.sharps, info.minor);
        int tonic = 0;
        bool minor = false;
        parseKey(out.key, tonic, minor);
        out.root = 60 + tonic - (tonic > 6 ? 12 : 0);
    }
    clip = std::move(out);
    return true;
}

std::vector<std::uint8_t> clipToFile(const Clip& clip, int ticksPerQuarter) {
    ticksPerQuarter = std::clamp(ticksPerQuarter, 24, 32767);
    struct Event {
        std::uint64_t tick;
        int order; // at one tick: note-offs, then note-ons
        std::uint8_t status, d1, d2;
    };
    std::vector<Event> events;
    const auto ticks = [&](double beats) { return static_cast<std::uint64_t>(std::llround(std::max(beats, 0.0) * ticksPerQuarter)); };
    for (const ClipNote& n : clip.notes) {
        const std::uint64_t on = ticks(n.start), off = std::max(ticks(n.start + n.length), on + 1);
        const auto key = static_cast<std::uint8_t>(std::clamp(n.key, 0, 127));
        const auto velocity = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(n.velocity * 127.0)), 1, 127));
        events.push_back({on, 1, 0x90, key, velocity});
        events.push_back({off, 0, 0x80, key, 0});
    }
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
    });

    std::vector<std::uint8_t> track;
    if (!clip.name.empty()) {
        track.push_back(0);
        track.push_back(0xFF);
        track.push_back(0x03);
        putVlq(track, static_cast<std::uint32_t>(clip.name.size()));
        track.insert(track.end(), clip.name.begin(), clip.name.end());
    }
    const auto micros = static_cast<std::uint32_t>(std::lround(60000000.0 / std::clamp(clip.tempo, 20.0, 999.0)));
    track.insert(track.end(), {0x00, 0xFF, 0x51, 0x03});
    putBe(track, micros, 3);
    track.insert(track.end(), {0x00, 0xFF, 0x58, 0x04, static_cast<std::uint8_t>(std::clamp(clip.beatsPerBar, 1, 16)), 0x02, 24, 8});
    int tonic = 0;
    bool minor = false;
    if (parseKey(clip.key, tonic, minor)) {
        const auto sf = static_cast<std::int8_t>(keySignature(tonic, minor));
        track.insert(track.end(), {0x00, 0xFF, 0x59, 0x02, static_cast<std::uint8_t>(sf), static_cast<std::uint8_t>(minor ? 1 : 0)});
    }
    std::uint64_t at = 0;
    for (const Event& e : events) {
        putVlq(track, static_cast<std::uint32_t>(e.tick - at));
        at = e.tick;
        track.push_back(e.status);
        track.push_back(e.d1);
        track.push_back(e.d2);
    }
    const std::uint64_t end = std::max(ticks(clip.lengthBeats()), at);
    putVlq(track, static_cast<std::uint32_t>(end - at));
    track.insert(track.end(), {0xFF, 0x2F, 0x00});

    std::vector<std::uint8_t> file = {'M', 'T', 'h', 'd'};
    putBe(file, 6, 4);
    putBe(file, 0, 2); // format 0
    putBe(file, 1, 2);
    putBe(file, static_cast<std::uint32_t>(ticksPerQuarter), 2);
    file.insert(file.end(), {'M', 'T', 'r', 'k'});
    putBe(file, static_cast<std::uint32_t>(track.size()), 4);
    file.insert(file.end(), track.begin(), track.end());
    return file;
}

} // namespace zed::midi
