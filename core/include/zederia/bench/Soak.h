// Soak renders (milestone 7 hardening): the engine plays for a long time under random parameter
// automation, random notes, controllers and preset switches, in blocks of random sizes. It must
// never output a value that is not a number, never run away in level, and never leave a voice
// sounding once every key is let go. Memory is measured by the caller (it depends on the
// platform): `progress` reports the start of every pass through the preset list, where the
// memory should be the same each time.
#pragma once

#include "zederia/content/Library.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace zed {

struct SoakProgress {
    double audioSeconds = 0.0; // rendered so far
    double wallSeconds = 0.0;
    int pass = 0;              // passes through the preset list, from 0
    int presetIndex = 0;       // position in the list
    std::string preset;        // the preset now loading ("init" when the list is empty)
    bool passStart = false;    // the first preset of a pass
};

struct SoakOptions {
    double seconds = 600.0;      // audio to render
    std::uint64_t seed = 1;
    std::vector<std::string> presets; // files to switch between, shuffled per pass; empty: the init patch
    double presetSeconds = 12.0; // each preset plays this long, then every key is let go and checked
    double movesPerSecond = 60.0; // random parameter moves (gain stages in dB are left alone)
    double notesPerSecond = 8.0;
    double sampleRate = 48000.0;
    int maxBlock = 1024;          // block sizes are drawn from 1 to this
    double runawayDbfs = 60.0;    // a block above this is a runaway (resonant filters may peak near +35 legitimately)
    double maxReleaseSeconds = 30.0; // voices still sounding this long after every key was let go are stuck
    bool realtimeQuality = true;  // the quality a host plays at (false: the offline quality)
    bool switchWhileHeld = true;  // half the presets arrive while keys are held (false: every key is let go first)
    std::vector<std::string> keepStill; // parameter id prefixes the moves leave alone ("arp.", "clip.")
    std::string tracePath; // when set: a text log of the notes sent and made, play-feature moves and preset loads
    ContentLibrary library;
    // Called before every preset; returning false stops the soak early.
    std::function<bool(const SoakProgress&)> progress;
};

struct SoakEvent {
    double atSeconds = 0.0;
    std::string preset;
    std::string what;
};

struct SoakReport {
    double audioSeconds = 0.0, wallSeconds = 0.0;
    int presetsLoaded = 0, passes = 0, moves = 0, notes = 0, releaseChecks = 0;
    int nonFiniteBlocks = 0;  // blocks whose output held a value that is not a number
    int outputRecoveries = 0; // times the engine had to mute such output
    int runawayBlocks = 0;
    int stuckReleases = 0;    // release checks that ended with voices still sounding
    int loadProblems = 0;     // presets that did not load or whose tables were missing
    double peakDbfs = -200.0;
    std::vector<SoakEvent> events; // what went wrong, at most 100
    bool ok() const { return nonFiniteBlocks == 0 && outputRecoveries == 0 && runawayBlocks == 0 && stuckReleases == 0; }
};

SoakReport runSoak(const SoakOptions& options);

} // namespace zed
