// The play features (plan 3.9, 6.1): arpeggiator order and timing for every mode, the step lane,
// chord memory, scale lock and strum, the clip player against its source MIDI file, MIDI out
// content, and the MIDI file reader and writer.
#include "../support/EngineRig.h"
#include "zederia/midi/MidiFile.h"
#include "zederia/play/Player.h"
#include "zederia/render/OfflineRenderer.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>

using namespace zed;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
constexpr std::int64_t kBeat = 24000; // samples per quarter note at 120 bpm

struct Played {
    int key = 0;
    std::int64_t on = 0, off = -1;
    float velocity = 0.0f;
};

// The notes the play features sent out, in the order they started.
std::vector<Played> notesOf(const RenderResult& r) {
    std::vector<Played> notes;
    std::map<int, std::size_t> open;
    for (const SeqEvent& e : r.midiOut) {
        if (e.event.type == MidiEvent::Type::NoteOn) {
            open[e.event.number] = notes.size();
            notes.push_back({e.event.number, e.sample, -1, e.event.value});
        } else if (e.event.type == MidiEvent::Type::NoteOff) {
            const auto it = open.find(e.event.number);
            if (it != open.end()) {
                notes[it->second].off = e.sample;
                open.erase(it);
            }
        }
    }
    return notes;
}

std::vector<int> keysOf(const std::vector<Played>& notes) {
    std::vector<int> keys;
    for (const Played& p : notes)
        keys.push_back(p.key);
    return keys;
}

std::vector<SeqEvent> chordHeld(std::vector<int> keys, std::int64_t from, std::int64_t to, float velocity = 0.8f) {
    std::vector<SeqEvent> events;
    for (const int k : keys)
        events.push_back({from, MidiEvent::noteOn(0, k, velocity)});
    for (const int k : keys)
        events.push_back({to, MidiEvent::noteOff(0, k)});
    std::stable_sort(events.begin(), events.end(), [](const SeqEvent& a, const SeqEvent& b) { return a.sample < b.sample; });
    return events;
}

RenderResult perform(const Patch& p, const std::vector<SeqEvent>& events, double tail = 0.5, std::uint64_t seed = 1) {
    RenderSettings settings;
    settings.tailSeconds = tail;
    settings.seed = seed;
    return renderOffline(p, events, settings);
}

Patch arpPatch(const char* mode, const char* rate = "1/16") {
    Patch p;
    p.set("arp.on", 1);
    p.set("arp.mode", Registry::get().find("arp.mode")->choiceIndex(mode));
    p.set("arp.rate", Registry::get().find("arp.rate")->choiceIndex(rate));
    p.set("arp.gate", 0.5);
    return p;
}

int choiceOf(const char* id, const char* token) { return Registry::get().find(id)->choiceIndex(token); }

} // namespace

TEST_CASE("play: arpeggiator order in every mode", "[play][arp]") {
    const auto run = [](Patch p, int octaves = 1, std::uint64_t seed = 1) {
        p.set("arp.octaves", octaves);
        return keysOf(notesOf(perform(p, chordHeld({60, 64, 67}, 0, 2 * kBeat), 0.5, seed)));
    };
    using V = std::vector<int>;
    CHECK(run(arpPatch("up")) == V{60, 64, 67, 60, 64, 67, 60, 64});
    CHECK(run(arpPatch("down")) == V{67, 64, 60, 67, 64, 60, 67, 64});
    CHECK(run(arpPatch("up_down")) == V{60, 64, 67, 64, 60, 64, 67, 64});
    CHECK(run(arpPatch("up_down_repeat")) == V{60, 64, 67, 67, 64, 60, 60, 64});
    CHECK(run(arpPatch("up"), 2) == V{60, 64, 67, 72, 76, 79, 60, 64});
    CHECK(run(arpPatch("down"), 2) == V{79, 76, 72, 67, 64, 60, 79, 76});

    // Chord: every held note on every step, an octave higher on alternate steps with two octaves.
    const auto chord = notesOf(perform(arpPatch("chord"), chordHeld({60, 64, 67}, 0, kBeat)));
    REQUIRE(chord.size() == 12);
    CHECK(chord[0].on == chord[2].on);
    CHECK(chord[3].on == 6000);
    Patch twoOctaves = arpPatch("chord");
    twoOctaves.set("arp.octaves", 2);
    const auto keys = keysOf(notesOf(perform(twoOctaves, chordHeld({60, 64, 67}, 0, kBeat / 2))));
    CHECK(keys == V{60, 64, 67, 72, 76, 79});

    // Random: only the held notes, the same sequence for the same seed, another for another seed.
    const V random = run(arpPatch("random"));
    for (const int k : random)
        CHECK((k == 60 || k == 64 || k == 67));
    CHECK(run(arpPatch("random")) == random);
    CHECK(run(arpPatch("random"), 1, 99) != random);
    // Walk: always to a neighbour.
    const V walk = run(arpPatch("walk"), 2);
    const V order = {60, 64, 67, 72, 76, 79};
    for (std::size_t i = 1; i < walk.size(); ++i) {
        const auto a = std::find(order.begin(), order.end(), walk[i - 1]) - order.begin();
        const auto b = std::find(order.begin(), order.end(), walk[i]) - order.begin();
        CHECK(std::abs(a - b) == 1);
    }
    // As played: the order the keys went down.
    std::vector<SeqEvent> pressed = {{0, MidiEvent::noteOn(0, 67, 0.8f)}, {10, MidiEvent::noteOn(0, 60, 0.8f)},
                                     {20, MidiEvent::noteOn(0, 64, 0.8f)}};
    for (const int k : {67, 60, 64})
        pressed.push_back({2 * kBeat, MidiEvent::noteOff(0, k)});
    CHECK(keysOf(notesOf(perform(arpPatch("as_played"), pressed))) == V{67, 60, 64, 67, 60, 64, 67, 60});
}

TEST_CASE("play: arpeggiator timing, gate, swing, latch and restart", "[play][arp]") {
    // On the song's grid at the step rate; the gate sets the length.
    const auto notes = notesOf(perform(arpPatch("up"), chordHeld({60, 64}, 0, kBeat)));
    REQUIRE(notes.size() == 4);
    for (std::size_t i = 0; i < notes.size(); ++i) {
        CHECK(notes[i].on == static_cast<std::int64_t>(i) * 6000);
        CHECK(notes[i].off == notes[i].on + 3000);
        CHECK(notes[i].velocity == Approx(0.8f).margin(0.01f));
    }
    // Swing delays every second step by up to half a step.
    Patch swung = arpPatch("up", "1/8");
    swung.set("arp.swing", 1.0);
    const auto s = notesOf(perform(swung, chordHeld({60, 64}, 0, 2 * kBeat)));
    REQUIRE(s.size() == 4);
    CHECK(s[0].on == 0);
    CHECK(s[1].on == 12000 + 6000);
    CHECK(s[2].on == 24000);
    CHECK(s[3].on == 36000 + 6000);
    // Triplet rate.
    const auto t = notesOf(perform(arpPatch("up", "1/8T"), chordHeld({60}, 0, kBeat)));
    REQUIRE(t.size() == 3);
    CHECK(t[1].on == 8000);
    CHECK(t[2].on == 16000);

    // A phrase that starts between grid lines starts at once, then follows the grid.
    const auto late = notesOf(perform(arpPatch("up"), chordHeld({60, 64}, 1000, kBeat)));
    REQUIRE(late.size() >= 3);
    CHECK(late[0].on == 1000);
    CHECK(late[1].on == 6000);
    CHECK(late[2].on == 12000);

    // Latch: the pattern goes on after the keys are released; a new chord replaces it.
    Patch latched = arpPatch("up");
    latched.set("arp.latch", 1);
    std::vector<SeqEvent> events = chordHeld({60, 64}, 0, kBeat / 2);
    const auto more = chordHeld({72}, 2 * kBeat, 2 * kBeat + 100);
    events.insert(events.end(), more.begin(), more.end());
    const auto l = notesOf(perform(latched, events, 1.0));
    std::vector<int> before, after;
    for (const Played& p : l)
        (p.on < 2 * kBeat ? before : after).push_back(p.key);
    CHECK(before.size() == 8); // two beats of steps although the keys went up after half a beat
    CHECK(std::all_of(after.begin(), after.end(), [](int k) { return k == 72; }));
    CHECK(after.size() >= 4);

    // Restart: a new phrase from silence starts the pattern over; without it the pattern goes on.
    for (const bool restart : {true, false}) {
        Patch p = arpPatch("up");
        p.set("arp.restart", restart ? 1 : 0);
        std::vector<SeqEvent> phrases = chordHeld({60, 64, 67}, 0, 12000); // two steps: 60, 64
        const auto second = chordHeld({60, 64, 67}, kBeat, kBeat + 12000);
        phrases.insert(phrases.end(), second.begin(), second.end());
        const auto k = keysOf(notesOf(perform(p, phrases)));
        REQUIRE(k.size() == 4);
        CHECK(k[2] == (restart ? 60 : 67));
    }

    // Without a running transport the steps count from the first key.
    Patch free = arpPatch("up");
    zedtest::EngineRig rig(free);
    MidiEvent out[256];
    int made = 0;
    std::vector<int> ons;
    for (int block = 0; block < 40; ++block) {
        std::vector<MidiEvent> in;
        if (block == 1)
            in = {MidiEvent::noteOn(100, 60, 0.8f), MidiEvent::noteOn(100, 64, 0.8f)};
        rig.left.resize(512);
        rig.right.resize(512);
        ProcessArgs args{rig.left.data(), rig.right.data(), 512, in, Transport{}};
        args.midiOut = out;
        args.midiOutCapacity = 256;
        args.midiOutCount = &made;
        rig.engine.process(args);
        for (int i = 0; i < made; ++i)
            if (out[i].type == MidiEvent::Type::NoteOn)
                ons.push_back(block * 512 + out[i].offset);
    }
    REQUIRE(ons.size() >= 3);
    CHECK(ons[0] == 612);
    CHECK(ons[1] == 612 + 6000);
    CHECK(ons[2] == 612 + 12000);
}

TEST_CASE("play: the step lane: rests, ties, transposition, velocity and ratchets", "[play][arp]") {
    Patch p = arpPatch("up");
    p.set("arp.steps", 4);
    p.set("arp.pattern", 3); // pattern slot 3
    ArpPattern& lane = p.arpPatterns[2];
    lane.steps = {ArpStep{}, ArpStep{StepState::Tie}, ArpStep{StepState::Rest}, ArpStep{StepState::On, 0.5, 12, 2}};
    const auto notes = notesOf(perform(p, chordHeld({60, 64}, 0, kBeat)));
    REQUIRE(notes.size() == 3);
    CHECK(notes[0].key == 60);
    CHECK(notes[0].on == 0);
    CHECK(notes[0].off == 6000 + 3000); // held through the tie
    CHECK(notes[1].key == 64 + 12);
    CHECK(notes[1].on == 18000);
    CHECK(notes[1].off == 18000 + 1500);
    CHECK(notes[1].velocity == Approx(0.4f).margin(0.01f));
    CHECK(notes[2].key == 64 + 12);
    CHECK(notes[2].on == 18000 + 3000); // the second ratchet
}

TEST_CASE("play: chord memory, scale lock and strum", "[play][chord]") {
    Patch p;
    p.set("chord.memory", 1);
    const auto chord = notesOf(perform(p, chordHeld({60}, 0, kBeat)));
    REQUIRE(chord.size() == 3);
    CHECK(keysOf(chord) == std::vector<int>{60, 64, 67});
    CHECK(chord[2].on == 0);
    CHECK(chord[2].off == kBeat);
    p.chordNotes = {0, 7, 12};
    CHECK(keysOf(notesOf(perform(p, chordHeld({50}, 0, kBeat)))) == std::vector<int>{50, 57, 62});

    // Scale lock: snap to the nearest note of C major (the lower one on a tie), or drop the note.
    Patch lock;
    lock.set("chord.scale_lock", 1);
    CHECK(keysOf(notesOf(perform(lock, chordHeld({61}, 0, kBeat)))) == std::vector<int>{60});
    CHECK(keysOf(notesOf(perform(lock, chordHeld({66}, 0, kBeat)))) == std::vector<int>{65});
    lock.set("chord.key", choiceOf("chord.key", "a"));
    lock.set("chord.scale", choiceOf("chord.scale", "minor_pentatonic"));
    CHECK(keysOf(notesOf(perform(lock, chordHeld({59}, 0, kBeat)))) == std::vector<int>{60}); // B -> C, the nearer
    lock.set("chord.lock_mode", choiceOf("chord.lock_mode", "filter"));
    CHECK(notesOf(perform(lock, chordHeld({59}, 0, kBeat))).empty());
    CHECK(keysOf(notesOf(perform(lock, chordHeld({60}, 0, kBeat)))) == std::vector<int>{60});

    // Strum: 10 ms between the notes of a chord, upward, downward or alternating.
    Patch strum;
    strum.set("chord.memory", 1);
    strum.set("chord.strum", 10.0);
    const auto up = notesOf(perform(strum, chordHeld({60}, 0, kBeat)));
    REQUIRE(up.size() == 3);
    CHECK(keysOf(up) == std::vector<int>{60, 64, 67});
    CHECK(up[1].on == 480);
    CHECK(up[2].on == 960);
    strum.set("chord.strum_direction", choiceOf("chord.strum_direction", "down"));
    CHECK(keysOf(notesOf(perform(strum, chordHeld({60}, 0, kBeat)))) == std::vector<int>{67, 64, 60});
    strum.set("chord.strum_direction", choiceOf("chord.strum_direction", "alternate"));
    std::vector<SeqEvent> two = chordHeld({60}, 0, kBeat);
    const auto again = chordHeld({60}, 2 * kBeat, 3 * kBeat);
    two.insert(two.end(), again.begin(), again.end());
    CHECK(keysOf(notesOf(perform(strum, two))) == std::vector<int>{60, 64, 67, 67, 64, 60});
    // Hand-played chords strum too, and a key let go before its turn never sounds.
    Patch hand;
    hand.set("chord.strum", 50.0);
    std::vector<SeqEvent> quick = {{0, MidiEvent::noteOn(0, 60, 0.8f)}, {0, MidiEvent::noteOn(0, 64, 0.8f)},
                                   {0, MidiEvent::noteOn(0, 67, 0.8f)}, {3000, MidiEvent::noteOff(0, 67)},
                                   {kBeat, MidiEvent::noteOff(0, 60)}, {kBeat, MidiEvent::noteOff(0, 64)}};
    CHECK(keysOf(notesOf(perform(hand, quick))) == std::vector<int>{60, 64});
}

TEST_CASE("play: the clip player follows the transport or a key", "[play][clip]") {
    Patch p;
    Clip& clip = p.clips[1];
    clip.bars = 1;
    clip.notes = {{0.0, 0.5, 60, 0.8}, {1.0, 0.5, 62, 0.6}, {2.5, 1.0, 64, 1.0}};
    p.set("clip.on", 1);
    p.set("clip.slot", 2);
    p.set("clip.trigger", choiceOf("clip.trigger", "transport"));
    // Two passes of the one-bar loop, on the song's beats.
    const auto transport = notesOf(perform(p, {}, 4.0));
    REQUIRE(transport.size() >= 6);
    const std::int64_t starts[] = {0, kBeat, 60000, 4 * kBeat, 5 * kBeat, 4 * kBeat + 60000};
    for (std::size_t i = 0; i < 6; ++i) {
        INFO("note " << i);
        CHECK(transport[i].on == starts[i]);
        CHECK(transport[i].key == clip.notes[i % 3].key);
        CHECK(transport[i].off - transport[i].on == static_cast<std::int64_t>(clip.notes[i % 3].length * kBeat));
        CHECK(transport[i].velocity == Approx(clip.notes[i % 3].velocity).margin(0.01));
    }
    // A key starts the clip transposed to it; letting go stops it.
    p.set("clip.trigger", choiceOf("clip.trigger", "key"));
    const auto key = notesOf(perform(p, chordHeld({62}, 1000, 1000 + 30000)));
    REQUIRE(key.size() == 2);
    CHECK(key[0].key == 62);
    CHECK(key[0].on == 1000);
    CHECK(key[1].key == 64);
    CHECK(key[1].on == 1000 + kBeat);
    CHECK(key[1].off == 1000 + 30000);
    // Latched, it carries on; without the loop it plays once.
    p.set("clip.latch", 1);
    p.set("clip.loop", 0);
    const auto once = notesOf(perform(p, chordHeld({60}, 0, 100), 4.0));
    CHECK(keysOf(once) == std::vector<int>{60, 62, 64});
}

TEST_CASE("play: a clip from a MIDI file plays back what the file holds", "[play][clip][midi]") {
    Clip written;
    written.name = "Riff";
    written.bars = 2;
    written.tempo = 96.0;
    written.key = "A minor";
    Rng rng(5);
    for (int i = 0; i < 24; ++i)
        written.notes.push_back({i * 1.0 / 3.0, 0.25, 45 + static_cast<int>(rng.next() % 24), (1 + rng.next() % 127) / 127.0});
    const auto file = midi::clipToFile(written);
    Clip read;
    std::string error;
    REQUIRE(midi::clipFromFile(file, read, error));
    CHECK(read.name == "Riff");
    CHECK(read.bars == 2.0);
    CHECK(read.tempo == Approx(96.0).margin(0.01));
    CHECK(read.key == "A minor");
    CHECK(read.root == 57);
    REQUIRE(read.notes.size() == written.notes.size());
    for (std::size_t i = 0; i < read.notes.size(); ++i) {
        CHECK(read.notes[i].start == Approx(written.notes[i].start).margin(0.5 / 960.0));
        CHECK(read.notes[i].key == written.notes[i].key);
        CHECK(read.notes[i].velocity == Approx(written.notes[i].velocity).margin(0.5 / 127.0));
    }
    // Played by the clip player at the host's tempo, every note starts where the file says.
    Patch p;
    p.clips[0] = read;
    p.set("clip.on", 1);
    p.set("clip.trigger", choiceOf("clip.trigger", "transport"));
    p.set("clip.loop", 0);
    const auto notes = notesOf(perform(p, {}, 4.5));
    for (const Played& n : notes)
        UNSCOPED_INFO("played " << n.key << " at " << n.on);
    REQUIRE(notes.size() == read.notes.size());
    for (std::size_t i = 0; i < notes.size(); ++i) {
        INFO("note " << i);
        CHECK(notes[i].key == read.notes[i].key);
        CHECK(notes[i].on == static_cast<std::int64_t>(std::ceil(read.notes[i].start * kBeat - 1e-6)));
        CHECK(notes[i].velocity == Approx(read.notes[i].velocity).margin(1e-6));
    }
}

TEST_CASE("play: MIDI out carries only what the play features made", "[play][midi]") {
    Patch plain;
    CHECK(perform(plain, chordHeld({60}, 0, kBeat)).midiOut.empty()); // nothing was made
    Patch arp = arpPatch("up");
    CHECK(perform(arp, chordHeld({60}, 0, kBeat)).midiOut.size() == 8); // four notes, on and off
    arp.set("global.midi_out", 0);
    CHECK(perform(arp, chordHeld({60}, 0, kBeat)).midiOut.empty());
    // The voices play what the arpeggiator makes: one note at a time.
    zedtest::EngineRig rig(arpPatch("up"));
    rig.transport.playing = true;
    rig.run(512, {MidiEvent::noteOn(0, 60, 0.8f), MidiEvent::noteOn(0, 64, 0.8f), MidiEvent::noteOn(0, 67, 0.8f)});
    rig.run(512);
    CHECK(rig.engine.activeVoices() == 1);
}

TEST_CASE("play: an arpeggio with effects renders the same every time", "[play][determinism]") {
    Patch p = arpPatch("random");
    p.addFxSlot(0, Registry::get().effectIndex("chorus"));
    p.addFxSlot(0, Registry::get().effectIndex("delay"));
    p.addFxSlot(2, Registry::get().effectIndex("reverb"));
    const auto events = chordHeld({57, 60, 64, 69}, 0, 4 * kBeat);
    const RenderResult a = perform(p, events, 2.0), b = perform(p, events, 2.0);
    CHECK(a.hash() == b.hash());
    CHECK(a.midiOut.size() == b.midiOut.size());
    RenderSettings blocks;
    blocks.tailSeconds = 2.0;
    blocks.blockSize = 64;
    CHECK(renderOffline(p, events, blocks).hash() == a.hash()); // and the same for another block size
}

TEST_CASE("midi: key names, signatures and damaged files", "[midi]") {
    int tonic = -1;
    bool minor = false;
    REQUIRE(midi::parseKey("A minor", tonic, minor));
    CHECK(tonic == 9);
    CHECK(minor);
    REQUIRE(midi::parseKey("bb", tonic, minor));
    CHECK(tonic == 10);
    CHECK_FALSE(minor);
    REQUIRE(midi::parseKey("F# min", tonic, minor));
    CHECK(midi::keySignature(tonic, minor) == 3);
    CHECK_FALSE(midi::parseKey("H major", tonic, minor));
    CHECK_FALSE(midi::parseKey("C dorian", tonic, minor));
    CHECK(midi::keySignature(4, false) == 4);  // E major
    CHECK(midi::keySignature(10, false) == -2); // B flat major
    CHECK(midi::keyName(-3, false) == "Eb major");
    CHECK(midi::keyName(3, true) == "F# minor");

    // A format 1 file written by hand: two tracks, running status, a note-on of velocity zero as
    // the release.
    const std::vector<std::uint8_t> file = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 2, 0x01, 0xE0, // 480 ticks per quarter
        'M', 'T', 'r', 'k', 0, 0, 0, 11, 0, 0xFF, 0x51, 3, 0x07, 0xA1, 0x20, 0, 0xFF, 0x2F, 0,
        'M', 'T', 'r', 'k', 0, 0, 0, 16, 0, 0x90, 60, 100, 0x83, 0x60, 60, 0, 0, 64, 90, 0x81, 0x70, 0x80, 64, 0};
    std::vector<ClipNote> notes;
    midi::FileInfo info;
    std::string error;
    // Track 2 length: 0 90 3C 64 | 83 60 3C 00 | 00 40 5A | 81 70 80 40 00 = 16 bytes; no end-of-track (tolerated).
    REQUIRE(midi::readNotes(file, notes, info, error));
    CHECK(info.tempo == Approx(120.0));
    REQUIRE(notes.size() == 2);
    CHECK(notes[0].key == 60);
    CHECK(notes[0].length == Approx(1.0));
    CHECK(notes[1].key == 64);
    CHECK(notes[1].start == Approx(1.0));
    CHECK(notes[1].length == Approx(0.5));

    // Damaged or foreign data is refused with a message, never a crash.
    CHECK_FALSE(midi::readNotes(std::vector<std::uint8_t>{'R', 'I', 'F', 'F'}, notes, info, error));
    CHECK_FALSE(error.empty());
    Rng rng(9);
    for (int trial = 0; trial < 2000; ++trial) {
        std::vector<std::uint8_t> bytes = file;
        const int flips = 1 + static_cast<int>(rng.next() % 6);
        for (int f = 0; f < flips; ++f)
            bytes[rng.next() % bytes.size()] = static_cast<std::uint8_t>(rng.next());
        if (rng.next() % 3 == 0)
            bytes.resize(rng.next() % bytes.size());
        Clip clip;
        (void)midi::clipFromFile(bytes, clip, error);
    }
}
