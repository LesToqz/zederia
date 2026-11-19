#include "zederia/render/OfflineRenderer.h"

#include "zederia/engine/Engine.h"
#include "zederia/util/Math.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace zed {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

bool parseNumber(std::string_view s, double& out) {
    s = trim(s);
    if (s.empty())
        return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size() && std::isfinite(out);
}

void sortTimeline(std::vector<SeqEvent>& events) {
    // Note-offs first at equal times, so a repeated key is released before it is struck again.
    std::stable_sort(events.begin(), events.end(), [](const SeqEvent& a, const SeqEvent& b) {
        if (a.sample != b.sample)
            return a.sample < b.sample;
        return a.event.type == MidiEvent::Type::NoteOff && b.event.type != MidiEvent::Type::NoteOff;
    });
}

} // namespace

int parseNoteName(std::string_view text) {
    text = trim(text);
    if (text.empty())
        return -1;
    static const int offsets[] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    const char letter = static_cast<char>(text[0] >= 'a' && text[0] <= 'g' ? text[0] - 'a' + 'A' : text[0]);
    if (letter < 'A' || letter > 'G')
        return -1;
    int semitone = offsets[letter - 'A'];
    std::size_t i = 1;
    while (i < text.size() && (text[i] == '#' || text[i] == 'b')) {
        semitone += text[i] == '#' ? 1 : -1;
        ++i;
    }
    int octave = 0;
    const auto r = std::from_chars(text.data() + i, text.data() + text.size(), octave);
    if (i >= text.size() || r.ec != std::errc() || r.ptr != text.data() + text.size())
        return -1;
    const int note = (octave + 1) * 12 + semitone;
    return note >= 0 && note <= 127 ? note : -1;
}

bool parseNoteList(std::string_view text, std::vector<NoteSpec>& out, std::string& error) {
    out.clear();
    std::size_t pos = 0;
    int entry = 0;
    while (pos <= text.size()) {
        const std::size_t comma = std::min(text.find(',', pos), text.size());
        const std::string_view item = trim(text.substr(pos, comma - pos));
        pos = comma + 1;
        if (item.empty())
            continue;
        ++entry;
        std::string_view fields[4];
        int count = 0;
        std::size_t start = 0;
        while (start <= item.size() && count < 4) {
            const std::size_t colon = std::min(item.find(':', start), item.size());
            fields[count++] = item.substr(start, colon - start);
            start = colon + 1;
        }
        const std::string where = "note " + std::to_string(entry) + " (\"" + std::string(item) + "\")";
        if (count < 3 || start <= item.size()) {
            error = where + " must look like note:start:duration or note:start:duration:velocity";
            return false;
        }
        NoteSpec note;
        double number = 0.0;
        if (parseNumber(fields[0], number) && number == std::floor(number) && number >= 0 && number <= 127)
            note.note = static_cast<int>(number);
        else if ((note.note = parseNoteName(fields[0])) < 0) {
            error = where + ": the note must be a MIDI number from 0 to 127 or a name such as C4";
            return false;
        }
        if (!parseNumber(fields[1], note.start) || note.start < 0 || note.start > 3600.0) {
            error = where + ": the start must be a time in seconds from 0 to 3600";
            return false;
        }
        if (!parseNumber(fields[2], note.duration) || note.duration <= 0 || note.duration > 3600.0) {
            error = where + ": the duration must be a time in seconds above 0";
            return false;
        }
        double velocity = 100.0;
        if (count == 4 && (!parseNumber(fields[3], velocity) || velocity < 1 || velocity > 127)) {
            error = where + ": the velocity must be from 1 to 127";
            return false;
        }
        note.velocity = velocity / 127.0;
        out.push_back(note);
    }
    if (out.empty()) {
        error = "the note list is empty";
        return false;
    }
    return true;
}

std::vector<SeqEvent> sequenceFromNotes(std::span<const NoteSpec> notes, double sampleRate) {
    std::vector<SeqEvent> events;
    events.reserve(notes.size() * 2);
    for (const auto& n : notes) {
        const auto on = static_cast<std::int64_t>(std::llround(n.start * sampleRate));
        const auto off = std::max(on + 1, static_cast<std::int64_t>(std::llround((n.start + n.duration) * sampleRate)));
        events.push_back({on, MidiEvent::noteOn(0, n.note, static_cast<float>(n.velocity))});
        events.push_back({off, MidiEvent::noteOff(0, n.note)});
    }
    sortTimeline(events);
    return events;
}

// --- Standard MIDI Files -----------------------------------------------------------------------

namespace {

struct Reader {
    std::span<const std::uint8_t> data;
    std::size_t pos = 0;
    bool ok = true;

    std::size_t left() const { return data.size() - pos; }
    std::uint8_t u8() {
        if (pos >= data.size()) {
            ok = false;
            return 0;
        }
        return data[pos++];
    }
    std::uint32_t be(int bytes) {
        std::uint32_t v = 0;
        for (int i = 0; i < bytes; ++i)
            v = (v << 8) | u8();
        return v;
    }
    std::uint32_t varlen() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const std::uint8_t b = u8();
            v = (v << 7) | (b & 0x7fu);
            if ((b & 0x80u) == 0)
                return v;
        }
        ok = false; // more than four bytes is not a valid length
        return v;
    }
    void skip(std::size_t n) {
        if (n > left())
            ok = false;
        else
            pos += n;
    }
};

struct TickEvent {
    std::uint64_t tick = 0;
    std::uint32_t order = 0;
    MidiEvent event;
};
struct TempoChange {
    std::uint64_t tick = 0;
    double microsPerQuarter = 500000.0;
};

} // namespace

bool sequenceFromMidiFile(std::span<const std::uint8_t> bytes, double sampleRate, std::vector<SeqEvent>& out,
                          std::string& error) {
    out.clear();
    constexpr std::size_t kMaxEvents = 2000000;
    Reader r{bytes};
    if (bytes.size() < 14 || std::memcmp(bytes.data(), "MThd", 4) != 0) {
        error = "this is not a MIDI file (it does not start with MThd)";
        return false;
    }
    r.pos = 4;
    const std::uint32_t headerLength = r.be(4);
    const std::uint32_t format = r.be(2);
    const std::uint32_t trackCount = r.be(2);
    const std::uint32_t division = r.be(2);
    if (headerLength < 6 || format > 1) {
        error = "only MIDI file formats 0 and 1 are supported";
        return false;
    }
    if ((division & 0x8000u) != 0 || division == 0) {
        error = "MIDI files with SMPTE time division are not supported; export with ticks per quarter note";
        return false;
    }
    r.skip(headerLength - 6);

    std::vector<TickEvent> events;
    std::vector<TempoChange> tempos;
    std::uint32_t order = 0;
    for (std::uint32_t track = 0; track < trackCount && r.ok && r.left() >= 8; ++track) {
        const bool isTrack = std::memcmp(bytes.data() + r.pos, "MTrk", 4) == 0;
        r.pos += 4;
        const std::uint32_t length = r.be(4);
        if (length > r.left()) {
            error = "the MIDI file is cut short (a track is longer than the file)";
            return false;
        }
        if (!isTrack) { // unknown chunk: skip it, as the format asks
            r.skip(length);
            --track;
            continue;
        }
        Reader t{bytes.subspan(r.pos, length)};
        r.skip(length);
        std::uint64_t tick = 0;
        std::uint8_t running = 0;
        while (t.ok && t.left() > 0) {
            tick += t.varlen();
            std::uint8_t status = t.u8();
            if (status < 0x80) { // running status: this byte is already data
                if (running == 0) {
                    t.ok = false;
                    break;
                }
                --t.pos;
                status = running;
            }
            if (status == 0xff) {
                const std::uint8_t type = t.u8();
                const std::uint32_t length2 = t.varlen();
                if (type == 0x51 && length2 == 3 && t.left() >= 3) {
                    const std::uint32_t micros = t.be(3);
                    if (micros > 0)
                        tempos.push_back({tick, static_cast<double>(micros)});
                } else {
                    t.skip(length2);
                }
                if (type == 0x2f)
                    break;
                continue;
            }
            if (status == 0xf0 || status == 0xf7) {
                t.skip(t.varlen());
                continue;
            }
            if (status >= 0xf0) { // other system messages do not belong in a file
                t.ok = false;
                break;
            }
            running = status;
            const std::uint8_t d1 = t.u8();
            const bool oneByte = (status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0;
            const std::uint8_t d2 = oneByte ? std::uint8_t{0} : t.u8();
            if (!t.ok)
                break;
            MidiEvent e;
            if (midiFromBytes(status, d1, d2, 0, e)) {
                if (events.size() >= kMaxEvents) {
                    error = "the MIDI file has too many events";
                    return false;
                }
                events.push_back({tick, order++, e});
            }
        }
        if (!t.ok) {
            error = "the MIDI file is damaged (track " + std::to_string(track + 1) + " could not be read)";
            return false;
        }
    }
    if (!r.ok) {
        error = "the MIDI file is cut short";
        return false;
    }

    std::stable_sort(tempos.begin(), tempos.end(), [](const TempoChange& a, const TempoChange& b) { return a.tick < b.tick; });
    std::stable_sort(events.begin(), events.end(), [](const TickEvent& a, const TickEvent& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
    });

    // Walk the tempo map once, converting ticks to seconds.
    double seconds = 0.0, microsPerQuarter = 500000.0;
    std::uint64_t lastTick = 0;
    std::size_t tempoIndex = 0;
    const auto advanceTo = [&](std::uint64_t tick) {
        while (tempoIndex < tempos.size() && tempos[tempoIndex].tick <= tick) {
            seconds += static_cast<double>(tempos[tempoIndex].tick - lastTick) * microsPerQuarter / 1e6 / division;
            lastTick = tempos[tempoIndex].tick;
            microsPerQuarter = tempos[tempoIndex].microsPerQuarter;
            ++tempoIndex;
        }
        seconds += static_cast<double>(tick - lastTick) * microsPerQuarter / 1e6 / division;
        lastTick = tick;
    };
    out.reserve(events.size());
    for (const auto& e : events) {
        advanceTo(e.tick);
        if (seconds > 24.0 * 3600.0) {
            error = "the MIDI file is longer than a day";
            return false;
        }
        out.push_back({static_cast<std::int64_t>(std::llround(seconds * sampleRate)), e.event});
    }
    sortTimeline(out);
    return true;
}

// --- Rendering ---------------------------------------------------------------------------------

std::uint64_t RenderResult::hash() const {
    std::uint64_t h = fnv1a(left.data(), left.size() * sizeof(float));
    return fnv1a(right.data(), right.size() * sizeof(float), h);
}

float RenderResult::peak() const {
    float peak = 0.0f;
    for (const float s : left)
        peak = std::max(peak, std::abs(s));
    for (const float s : right)
        peak = std::max(peak, std::abs(s));
    return peak;
}

RenderResult renderOffline(const Patch& patch, std::span<const SeqEvent> events, const RenderSettings& settings) {
    const int blockSize = std::clamp(settings.blockSize, 1, 8192);
    Engine engine;
    engine.setSeed(settings.seed);
    engine.prepare(settings.sampleRate);
    engine.setOffline(settings.offlineQuality);
    engine.loadPatch(patch);
    for (int i = 0; i < kNumOscs; ++i)
        if (settings.tables[static_cast<std::size_t>(i)] != nullptr)
            engine.setTable(i, settings.tables[static_cast<std::size_t>(i)]);
    if (settings.texture != nullptr)
        engine.setTexture(settings.texture);

    const std::int64_t lastEvent = events.empty() ? 0 : std::max<std::int64_t>(events.back().sample, 0);
    const double tail = settings.tailSeconds >= 0.0 ? settings.tailSeconds : engine.tailSeconds();
    const std::int64_t total =
        std::max(lastEvent + static_cast<std::int64_t>(std::ceil(tail * engine.sampleRate())) + 1,
                 static_cast<std::int64_t>(std::ceil(std::max(0.0, settings.minSeconds) * engine.sampleRate())));
    const auto progressEvery = static_cast<std::int64_t>(engine.sampleRate() * 0.05);
    std::int64_t nextProgress = progressEvery;

    RenderResult result;
    result.sampleRate = engine.sampleRate();
    result.left.assign(static_cast<std::size_t>(total), 0.0f);
    result.right.assign(static_cast<std::size_t>(total), 0.0f);

    std::vector<MidiEvent> block;
    block.reserve(256);
    std::vector<MidiEvent> midiOut(2048);
    std::size_t next = 0;
    Transport transport;
    transport.bpm = settings.bpm;
    transport.playing = true;
    for (std::int64_t pos = 0; pos < total; pos += blockSize) {
        const int frames = static_cast<int>(std::min<std::int64_t>(blockSize, total - pos));
        block.clear();
        while (next < events.size() && events[next].sample < pos + frames) {
            MidiEvent e = events[next].event;
            e.offset = static_cast<int>(std::max<std::int64_t>(events[next].sample - pos, 0));
            block.push_back(e);
            ++next;
        }
        transport.ppqPosition = static_cast<double>(pos) / engine.sampleRate() * settings.bpm / 60.0;
        ProcessArgs args{result.left.data() + pos, result.right.data() + pos, frames, block, transport};
        args.midiOut = midiOut.data();
        args.midiOutCapacity = static_cast<int>(midiOut.size());
        int made = 0;
        args.midiOutCount = &made;
        engine.process(args);
        for (int i = 0; i < made; ++i)
            result.midiOut.push_back({pos + midiOut[static_cast<std::size_t>(i)].offset, midiOut[static_cast<std::size_t>(i)]});
        if (settings.progress && pos + frames >= nextProgress) {
            nextProgress += progressEvery;
            if (!settings.progress(static_cast<double>(pos + frames) / static_cast<double>(total))) {
                result.cancelled = true;
                result.left.resize(static_cast<std::size_t>(pos + frames));
                result.right.resize(static_cast<std::size_t>(pos + frames));
                break;
            }
        }
    }
    return result;
}

// --- WAV ---------------------------------------------------------------------------------------

std::vector<std::uint8_t> encodeWav(std::span<const float> left, std::span<const float> right, double sampleRate,
                                    WavFormat format) {
    const std::size_t frames = std::min(left.size(), right.size());
    const std::uint32_t bytesPerSample = format == WavFormat::Pcm16 ? 2 : format == WavFormat::Pcm24 ? 3 : 4;
    const std::uint32_t channels = 2;
    const auto dataBytes = static_cast<std::uint32_t>(frames * channels * bytesPerSample);
    const bool isFloat = format == WavFormat::Float32;

    std::vector<std::uint8_t> out;
    out.reserve(dataBytes + 64);
    const auto tag = [&](const char* s) { out.insert(out.end(), s, s + 4); };
    const auto u16 = [&](std::uint32_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    const auto u32 = [&](std::uint32_t v) {
        u16(v & 0xffffu);
        u16(v >> 16);
    };
    const std::uint32_t factBytes = isFloat ? 12 : 0;
    tag("RIFF");
    u32(4 + (8 + 16) + factBytes + (8 + dataBytes));
    tag("WAVE");
    tag("fmt ");
    u32(16);
    u16(isFloat ? 3 : 1);
    u16(channels);
    u32(static_cast<std::uint32_t>(std::lround(sampleRate)));
    u32(static_cast<std::uint32_t>(std::lround(sampleRate)) * channels * bytesPerSample);
    u16(channels * bytesPerSample);
    u16(bytesPerSample * 8);
    if (isFloat) {
        tag("fact");
        u32(4);
        u32(static_cast<std::uint32_t>(frames));
    }
    tag("data");
    u32(dataBytes);
    for (std::size_t i = 0; i < frames; ++i) {
        for (const float sample : {left[i], right[i]}) {
            if (isFloat) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &sample, sizeof bits);
                u32(bits);
                continue;
            }
            const double clipped = std::clamp(static_cast<double>(sample), -1.0, 1.0);
            if (format == WavFormat::Pcm16) {
                u16(static_cast<std::uint32_t>(std::lround(clipped * 32767.0)) & 0xffffu);
            } else {
                const auto v = static_cast<std::uint32_t>(std::lround(clipped * 8388607.0));
                out.push_back(static_cast<std::uint8_t>(v));
                out.push_back(static_cast<std::uint8_t>(v >> 8));
                out.push_back(static_cast<std::uint8_t>(v >> 16));
            }
        }
    }
    return out;
}

bool writeFile(const std::string& path, std::span<const std::uint8_t> bytes, std::string& error) {
    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path.c_str(), "wb") != 0)
        f = nullptr;
#else
    f = std::fopen(path.c_str(), "wb");
#endif
    if (f == nullptr) {
        error = "cannot open " + path + " for writing";
        return false;
    }
    const bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed)
        error = "cannot write " + path;
    return ok && closed;
}

bool readFile(const std::string& path, std::vector<std::uint8_t>& bytes, std::string& error) {
    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path.c_str(), "rb") != 0)
        f = nullptr;
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (f == nullptr) {
        error = "cannot open " + path;
        return false;
    }
    bytes.clear();
    std::uint8_t buffer[65536];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) {
        bytes.insert(bytes.end(), buffer, buffer + n);
        if (bytes.size() > 256u * 1024u * 1024u)
            break;
    }
    const bool failed = std::ferror(f) != 0 || bytes.size() > 256u * 1024u * 1024u;
    std::fclose(f);
    if (failed)
        error = "cannot read " + path + " (unreadable or larger than 256 MB)";
    return !failed;
}

} // namespace zed
