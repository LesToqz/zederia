# Status

## Done
- Milestone 0 (environment): toolchain verified, versions pinned, smoke build passed, repo files created.
- Milestone 1 (foundation), tagged `m1-done`:
  - Build: CMake presets `debug`, `release`, `relwithdebinfo` (Ninja, 3 jobs); dependencies fetched at the pinned versions into a source cache shared by the presets (`build/_deps`); warnings are errors for our own code; `scripts/build.ps1`, `test.ps1`, `bench.ps1`, `install-dev.ps1`.
  - `zederia_core`: parameter registry (864 parameters in 123 modules, 760 exposed to the host; 13 effect types; modulation sources; tempo divisions), patch model with JSON load, save, validation and migration, engine skeleton (voice allocator, sample-accurate MIDI, 16-sample control tick, placeholder band-limited saw through the amp envelope), offline renderer (note list or Standard MIDI File to WAV), benchmark scenarios.
  - Plug-in "Zederia": VST3, Standalone and CLAP; stable parameter ids; state save and restore; latency 0 and tail reported; temporary editor (searchable parameter list, keyboard, level meter).
  - `zederia-cli` (`render`, `validate-patch`, `params`, `bench`) and `zederia-mcp` (prints its version).
  - Tests: 79 (core, plug-in, command-line), all under the audio-thread guard.
  - Docs: `docs/ARCHITECTURE.md`, generated `docs/PARAMETERS.md`.
- Milestone 2 (sound sources and wavetable toolkit), tagged `m2-done`:
  - Wavetable model: 2048-sample frames, 1 to 256 key frames, five morph modes, generated positions, Stepped flag, WAV files with a `ZEDT` metadata chunk, other frame sizes detected or chosen on import, damaged files refused.
  - Wavetable oscillator: 28 band-limited levels read with a six-point spline; smoothed position; all warp modes in two slots; phase controls; fixed pitch; cross-modulation (phase, amplitude, ring) between the three oscillators, Sub and Texture, loops included.
  - Unison up to 16 voices in 4- and 8-lane SIMD kernels, bit-identical to the scalar reference kernel.
  - Classic engine, Sub and Texture, each with level, pan and sends into two placeholder filter slots (serial or parallel) and a direct bus.
  - Quality modes Draft, Standard, High, Ultra: 1, 2, 4, 8 times oversampling of the paths that can alias, zero added latency; offline renders use the offline quality.
  - Lock-free hand-over of tables, texture samples and warp curves to the audio thread, with a 256-sample crossfade in playing voices.
  - Wavetable toolkit as library functions: frame and harmonic operations, frame management, morph generation, sorting, the formula language, imports (single cycles, pitch-tracked or fixed-length recordings, spectral snapshots, images) and resynthesis of a warp or a whole patch.
  - `zederia-cli`: `table info|new|formula|process|harmonic|frames|morph|sort|import|resynth|export|build`, `texture build`, `content build`; `render --content` and `--live-quality`; `bench --report-only`.
  - Content: 12 wavetable recipes and 6 texture recipes in `content/`, built into the factory folder (about 7 MB) by `scripts/install-dev.ps1`.
  - Plug-in: tables and the texture load by reference from the factory and user folders; the temporary editor has a picker for each oscillator's table and for the texture.
  - Tests: 160 (core, plug-in, command-line), all under the audio-thread guard.
- Milestone 3 (shaping and motion), tagged `m3-done`:
  - Two filter slots per voice with all 70 types of plan 3.5 in 16 families (state-variable, ladder, dual, morph, comb, flange, phase, formant, shelf and bell, ring modulator, sample-and-hold, all-pass cluster, diffuser, band-reject, howl, grit comb), built from zero-delay-feedback parts. Character per type, stereo offset, key tracking, Mix or Level, serial-to-parallel routing with the per-source sends. Saturating stages and nonlinear cores are oversampled by the quality mode. `Engine::filterView` and `filterResponse` give magnitude and phase at the current, modulated settings.
  - Four envelopes: delay, attack, hold, decay, sustain, release with a curve on attack, decay and release; any time can be a note length that follows the tempo; envelopes 2 to 4 have a multi-segment mode (32 points, sustain point, loop region).
  - Eight LFOs: drawn shapes (points and curves), four modes, free, synced and audio-rate rates, Anchor to the song position, phase, delay, rise, smooth, polarity, per voice or shared, key-tracked rate. Two Flux generators with four types, free or synced, shared or per voice.
  - Sources: eight macros (optionally bipolar), velocity, release velocity, note, pitch bend, mod wheel, channel and poly aftertouch, MPE pressure and slide, expression, breath, sustain pedal, any controller (`ccN`), Random 1 and 2, Alternate, voice index, unison index, constant, the audio outputs of the five sources, an envelope follower (the voices or a side-chain input) and bar phase.
  - A 64-route matrix: Via (invertible), amount, polarity, curve (linear, exponential, logarithmic, S, drawn), enable, mute, solo. Routes are polyphonic when a per-voice source or Via meets a per-voice destination. Any continuous parameter is a destination, including modulator rates and other routes' amounts.
  - Voice: glide (time, curve, always or legato-only, scaled), bend ranges, MPE (per-note bend, pressure, slide on member channels), master tune and reference pitch, Scala `.scl`/`.kbm` and `.tun` files with a tuning lock, Character (drift, four kinds of variance, warmth, noise floor), and a 5 ms glide on every continuous parameter with per-sample ramps behind it.
  - Patch: routes with drawn curves, LFO shapes, multi-segment envelopes, tempo-linked times, warp curves and a tuning are read, written and handed to the engine by `Engine::loadPatch` (the plug-in and the renderer use it).
  - Four hand-written test patches (`tests/data/patches/bass|pad|pluck|lead.zpreset`) that load from JSON and render through `zederia-cli`.
  - Tests: 225 (core, plug-in, command-line), all under the audio-thread guard.

## Measurements
### Milestone 3
Measured 2026-11-14 and 2026-11-15 on the machine in `docs/ENVIRONMENT.md`, Release build unless stated.

| What | Result | Target |
|---|---|---|
| Tests | 225 of 225 pass in Release (27 s) and in Debug (204 s) | all |
| pluginval v1.0.4, strictness 10, Release VST3 | SUCCESS | pass |
| State-variable types against their analog prototypes | worst 0.015 dB over the whole band (frequency-warped curve); 0.20 dB against the unwarped curve up to 2 kHz | 0.5 dB |
| Shelves and bell against the analytic curve | worst 0.00003 dB | 0.5 dB |
| Every linear type against the response given to the display | worst 0.27 dB (diffuser) | 0.5 dB |
| Ladder against its response at a small level | worst 0.0004 dB | 0.5 dB |
| A voice's filter (ten types, in the engine) against `Engine::filterResponse` | worst 0.0007 dB | 0.5 dB |
| Stability, filter alone: ten seconds of noise at full scale, maximum resonance, cutoff modulated at audio rate, all 70 types (Standard; two seconds each in Draft and High) | all finite; highest peak 56 (svf_peak24) | finite |
| Stability, whole voice: all 70 types in both slots at full resonance and drive, cutoffs moved by an oscillator and by an audio-rate LFO while an envelope sweeps them | all finite; highest peak 37 (howl_lp) | finite |
| Ladder self-oscillation pitch, 55 Hz to 7 kHz, at 1, 2 and 4 times oversampling | worst error 0.026 cent | in tune |
| Driven filter, 5 kHz sine, alias of the fifth harmonic | -39.2 dBc in Draft, -72.1 dBc in Standard | falls with quality |
| Envelope segment times (delay, attack, hold, decay, release; straight and curved; multi-segment points, sustain point and loop; tempo-linked at 60, 120 and 150 bpm) | every crossing within 1 ms or 1 % | 1 ms or 1 % |
| LFO phase one sixteenth after each of three bar lines, five divisions at 120 bpm | within 0.003 of the full range; a cycle that divides the bar starts over on the bar line; Anchor follows a tempo change and a jump of the song position | in phase |
| Matrix arithmetic (amount, both polarities, four curves and a drawn one, Via and inverted Via, clamping, sums, routes on routes) | the destination's live value equals the arithmetic to 1e-4 | exact |
| MPE | a bend on one channel moves only that channel's note (the chord equals the sum of the two notes rendered alone to 1e-4); pressure and slide read per note | per note |
| Microtuning, just scale, nine keys from C2 to C7 | worst error 0.0009 cent | 0.1 cent |
| Zipper noise: 101 continuous controls swept over their full range in 50 ms and back | worst jump 1.36 times the steady sound's (a macro throwing the cutoff across 5.7 octaves); limit 1.5 | no steps |
| Block-size independence with filters, a full 64-route matrix, audio-rate routes and a multi-segment envelope | identical hash at block sizes 512, 1, 16, 37 and 4096 | identical |
| Audio-thread guard with routes, LFO shapes, envelope shapes, tempo-linked times and the tuning replaced every few blocks while every parameter moves | no allocation, free or lock; output always finite | real-time safe |
| Test patches through `zederia-cli render` | bass, pad, pluck and lead load with no warnings, render twice to identical files, and pass the checks in `test_patches.cpp` (pitch, level, envelope shape, tempo, glide, vibrato, stereo width); nobody listened | render sensibly |

Benchmarks (Release, one core, 48 kHz, 512-sample blocks; baseline in `tests/bench/baseline.json`, best of 5 runs of 20 s by `scripts/bench.ps1 -UpdateBaseline`):

| Scenario | Realtime factor | Budget (plan 5.8) |
|---|---|---|
| idle | 0.026 % | 0.2 % |
| one_voice (one wavetable oscillator) | 0.195 % | 0.5 % |
| chord8 (8 voices, one oscillator at unison 7, one filter) | 3.09 % | 4 % |
| chord8_mod (the same with a per-voice LFO, an envelope and velocity on the cutoff and a macro on position) | 3.10 % | 4 % (not a plan row) |
| full8 (8 voices, 3 oscillators at unison 16, two filters; the six effects arrive in milestone 4) | 15.1 % | 35 % |
| full8_draft (the same in Draft) | 15.2 % | 20 % |

All budgets are met. Against milestone 2: `one_voice` rose from 0.171 % to 0.195 % (every voice now runs its envelopes, sources and ramps each control tick), and `chord8` from 2.42 % to 3.09 %, split about evenly between the filter it now has and the per-voice control work. Modulation costs nothing measurable in `chord8_mod`. Draft is no cheaper than Standard in `full8` because plain wavetable oscillators and linear filters are never oversampled. The machine was at normal speed (`idle` near its earlier value).

### Milestone 2
Measured 2026-11-12 and 2026-11-13 on the machine in `docs/ENVIRONMENT.md`, Release build unless stated.

| What | Result | Target |
|---|---|---|
| Tests | 160 of 160 pass in Release (29 s) and in Debug (272 s) | all |
| pluginval v1.0.4, strictness 10, Release VST3 | SUCCESS | pass |
| Aliasing, wavetable sawtooth with all 1024 harmonics, swept C0 to C8 at 44.1 kHz, worst non-harmonic component | -105.3 dBc in Draft, Standard, High and Ultra | -80 dBc (Draft), -96 dBc (others) |
| The same through the oversampled path with a neutral warp | -108.3 dBc | -96 dBc |
| Hard sync, worst alias below 20 kHz | Draft -24.9, Standard -36.6, High -44.0, Ultra -50.7 dBc | falls with quality |
| Classic saw, worst alias below 20 kHz, up to C5 / C5 to A6 | Draft -44.4 / -34.0, Standard -66.5 / -55.9, High -87.3 / -76.1 dBc | falls with quality |
| Tuning, wavetable oscillator, C0 to C8 | worst error 0.0073 cent | 0.1 cent |
| Tuning, Classic engine, C0 to C8 | worst error 0.004 cent | 0.1 cent |
| Unison loudness, 1 to 16 voices against one voice | -0.23 to +0.29 dB | within 1.5 dB |
| Position sweep across a smooth table | largest sample step no higher than the steady tone's (0.081); a Stepped table jumps (1.25) as it should | no clicks |
| SIMD kernels against the scalar reference | bit-identical, 4 and 8 lanes, every warp mode | identical |
| Half-band decimators, stop band | 104 dB (normal), 116 dB (steep) | 96 dB |
| Table or sample swapped from another thread while audio runs | no allocation, free or lock on the audio thread; no click | real-time safe |
| Starter content | 12 wavetables and 6 textures build from their recipes, byte-identical between builds | 12 and 6 |
| Standalone | each of Osc 1, Osc 2, Osc 3, Sub and Texture switched on alone from the temporary editor moved the level meter while a key was held; all five off left it empty; a factory table and a factory texture were chosen from the pickers and played (checked from window captures; nobody listened) | plays all five |

Benchmarks (Release, one core, 48 kHz, 512-sample blocks; baseline in `tests/bench/baseline.json`):

| Scenario | Baseline: best of 5 runs of 20 s (`scripts/bench.ps1 -UpdateBaseline`) | Budget (plan 5.8) |
|---|---|---|
| idle | 0.023 % | 0.2 % |
| one_voice (one wavetable oscillator) | 0.171 % | 0.5 % |
| chord8 (8 voices, one oscillator at unison 7; the filter joins in milestone 3) | 2.42 % | 4 % |

All three budgets are met. Other runs at normal speed gave 2.23 % and 2.30 % for `chord8`. The baseline was recalibrated once for this milestone (the milestone 1 numbers were for the placeholder oscillator).

For part of 2026-11-13 this virtual machine ran at about half speed: the same scenarios read 0.04 %, 0.36 % and 4.8 to 5.0 %, and a milestone 1 test whose code has not changed (the patch fuzz test, Debug, run alone) took 210 s against 111 s the day before. Readings taken in that state are not used. See Known issues.

### Milestone 1
Measured 2026-11-12 on the machine in `docs/ENVIRONMENT.md`.

| What | Result |
|---|---|
| Debug from an empty build folder | configure 67 s, build 180 s; 79 of 79 tests pass (111 s) |
| Release from an empty build folder | configure 68 s, build 258 s; 79 of 79 tests pass (10 s) |
| RelWithDebInfo from an empty build folder | configure 94 s, build 296 s (tests not run on this preset) |
| pluginval v1.0.4, strictness 10, Release VST3 | SUCCESS |
| Tuning, placeholder oscillator, C0..C8 at 44.1, 48 and 96 kHz | worst error 8.4e-10 cent (requirement: 0.1 cent) |
| Render determinism | `zederia-cli render` twice: identical SHA-256 (`128c54e7...5828` for the test phrase); output is also bit-identical across block sizes 1 to 4096 |
| Malformed-patch fuzzing | 10,000 documents, no crash; every document that loads is in range and round-trips byte-identically |
| Audio-thread guard | no allocation, free or lock inside the callback across all tests (engine and plug-in `processBlock`) |
| Standalone | launched, audio device opened; a mouse press on the on-screen keyboard showed "1 voice" and a moving meter, release returned to "0 voices" (checked from window captures; nobody listened) |

Benchmarks at milestone 1 (Release, one core, 48 kHz, 512-sample blocks, best of 5 runs of 20 s; since replaced as the baseline):

| Scenario | Realtime factor | Budget (plan 5.8) |
|---|---|---|
| idle | 0.014 % | 0.2 % |
| one_voice | 0.047 % | 0.5 % |
| chord8 (placeholder oscillator, no unison or filter yet) | 0.110 % | 4 % |

Milestone 0: pluginval on the smoke VST3 at strictness 5: SUCCESS. Smoke configure 9 s, build 201 s.

## Decisions
- MSVC from VS 2022 Build Tools (v143). Build with 3 jobs because of 7.9 GB RAM.
- CLAP via clap-juce-extensions (pinned commit). It brings CLAP SDK 1.2.7 as a submodule, not the 1.2.10 guessed in milestone 0; `cmake/Versions.cmake` now records 1.2.7.
- **The core has no plug-in framework dependency.** The plan allows the framework's non-GUI modules for file I/O, FFT and zlib; milestone 1 needed only a WAV writer and a MIDI-file reader, which are small and written in the core. This keeps the core quick to build and test and lets the CLI and server link it without the framework. Milestone 2 must decide how FFT, deflate and image decoding reach the core (own code, or a thin layer over the framework compiled once).
- **Dependencies are fetched by `cmake/Dependencies.cmake` into `build/_deps`**, one folder per pinned version, instead of per build folder, so the three presets download once. cpp-httplib is pinned but not fetched until milestone 6 uses it.
- **Patch layout follows the plan's example**: performance settings (mode, polyphony, glide, bend range, master) are under `global`; `voice` holds stealing, priority, filter routing and character. `bend_range` is a two-element list.
- **A full save writes every value**, so changing a default later cannot change saved sounds; `compact` omits defaults. Validation: errors fail the load, warnings (unknown fields, clamped values) do not.
- **Effect parameters are declared now** (13 types, at most 16 parameters each), mapped in order onto each slot's generic `p1..p16`. Slot values are stored in the effect parameter's own unit. The filter effect's type field is `filter_type`, because `type` names the effect.
- **Tempo-linked envelope times** (typed as `1/8`, `3/16`) are not parameters: milestone 3 adds them as text fields in the patch, since arbitrary fractions do not fit a fixed list. Synced LFO, Flux, arpeggiator and delay rates use the shared division list.
- **Non-automatable parameters** (voice mode, polyphony, engine switches, quality and similar) are in the registry and the patch but not exposed to the host.
- **Host parameter ids** are the framework's hash of the string id with the top bit cleared; the registry computes the same value and tests check both for collisions and that the two agree.
- **Control ticks sit on a grid fixed in absolute time**, with ramps between ticks, so output does not depend on the host block size.
- **Benchmark pass rule**: fail when over budget, or slower than baseline x 1.10 + 0.02 percentage points (the allowance keeps timer noise on near-zero loads from failing the step).
- **Defaults chosen where the plan is silent**: filter cutoff 2000 Hz, filter Character 50 %, oscillator level -6 dB, phase random 100 %, envelope decay and release curves 0.5, output protection off. Later milestones may revisit them while no presets exist.
- Commits use the repository's existing identity (`zederia <dev@localhost>`), since no git user is configured on this machine.

Milestone 2:
- **FFT, inflate and image decoding are the core's own code** (`dsp/Fft`, `util/Image`), so the core still has no plug-in framework dependency. The FFT is radix-2 with a chirp transform for other lengths; the image reader handles PNG, BMP and PNM. Writing deflate is not needed yet (the plug-in state uses the framework's).
- **One read kernel for every quality mode.** The plan's per-mode interpolators are replaced by one six-point spline whose error is compensated in the stored data; it meets the strictest target (-96 dBc) even in Draft, so the modes differ only in oversampling.
- **Level selection is strict**: a level is used only if none of its harmonics lies above half the sample rate, with about three levels per octave, so the top of the spectrum is never more than a third of an octave short. Nothing is left to alias and be filtered later.
- **A plain wavetable oscillator is never oversampled**; oversampling applies to warped or modulated oscillators, to modulators and to the Classic engine.
- **Classic engine**: a polynomial correction at the steps, oversampled by the quality mode; the saw falls so the four shapes share the phase of their fundamental. It takes the amplitude warps and the modulations; phase-reshaping warps are off for it.
- **Cross-modulation order**: Sub, Texture, then the oscillators in dependency order. A loop or self-modulation costs one base-rate sample of delay and runs sample by sample.
- **Stored positions**: the key frames themselves for none, crossfade and Stepped tables; 256 generated positions for the spectral morph modes.
- **A file without metadata whose length fits several frame sizes is read as 2048-sample frames first**; `--frame-size` on import overrides it.
- **The warp curve shape** (for the curve warps) is set on the engine only; its patch field arrives with the shape model in milestone 3.
- **SIMD**: SSE2 and AVX2 kernels on x86-64, chosen by lane count at run time; other processors use a portable 4-lane fallback until a NEON kernel is written.
- **Factory content is built, not stored**: recipes in the repository, audio made by `zederia-cli content build`. Embedding it in the binary waits for the installer milestone.
- **Paths are passed in the system's narrow encoding**; see Known issues.
- **`cli.bench_runs` only checks that the scenarios run** (`bench --report-only`); budgets and regressions are enforced by `scripts/bench.ps1`. A half-second run inside a parallel test run, or in a Debug build, says nothing about the budgets.
- **Baseline recalibrated once** to the milestone 2 sources, as the milestone allows.

Milestone 3:
- **Parameter path.** Every continuous parameter glides for 5 ms in its own (skewed) scale at the control rate; routes are added after the glide, so a fast LFO is not slowed by the smoothing. Everything that sounds then moves in a straight line per sample to the value for the next control tick.
- **Modulators are computed one control interval ahead** (their value at the next tick), so ramps arrive exactly and nothing lags; what the engine reports to other threads is that look-ahead value.
- **Route arithmetic**: `signal = curve(polarity(source)) * via`, added to the destination in normalized units and clamped to its range. "Bi" recentres a 0..1 source and leaves a -1..1 source alone; the curve keeps the sign; Via reads its source as 0..1.
- **Polyphonic or shared.** A route is per voice when its source or its Via is per voice and its destination belongs to a voice module (oscillators, Sub, Texture, filters, envelopes, LFOs, `voice.*`). Otherwise it is evaluated once; a per-voice source on such a route reads the most recent voice. LFO and Flux scope decide which they are.
- **Audio-rate modulation is per sample only onto filter cutoffs** (the audio output of a source or an audio-rate LFO; four such routes per filter). On other destinations those sources are read once per control tick. Oscillator-to-oscillator modulation at audio rate is the warp slots' job (milestone 2).
- **Unison index** acts per lane, so it reaches only what exists per lane: an oscillator's position, warp amounts, pan, fine and coarse pitch (16 routes per oscillator).
- **Voice index** is the number of voices already sounding when the note starts, divided by polyphony minus one.
- **Depths the plan leaves open**: stereo offset moves each side one octave at 100 %; drift wanders up to 25 cents; cutoff variance one octave either way; level variance 6 dB either way; envelope variance half to twice the time; pan variance up to hard left or right; noise floor -40 dB at 100 % (square law); filter key tracking pivots on middle C and follows the pitch wheel.
- **Glide** moves the key position and the tuning turns that into pitch, so a slide passes through the tuned pitches. In Poly mode a note slides from the previous note's pitch.
- **A shared LFO in Retrigger mode restarts with every note** of any voice; in Free mode nothing restarts it. A per-voice Free LFO joins the phase it would have had if it had run since the start.
- **MPE** uses the lower zone: channel 1 is the master channel (its bend uses the ordinary bend range and reaches every note), channels 2 to 16 carry one note each with bend (`global.mpe_bend_range`), pressure and controller 74.
- **Tuning** is stored as a pitch per key; the lock is an engine setting (`setTuningLocked`), not a parameter: it belongs to the instrument, not to a patch, and gets its place in the settings file with the interface.
- **Filters are redesigned only when a voice's settings change**, and their ramps advance by repeated addition, so the output is bit-identical for any host block size with filters and modulation too.
- **Warmth** is a cubic saturation at the base rate (third harmonic only at moderate levels) with a 3.5 kHz tilt; it is not oversampled.
- **Zipper check**: the largest sample-to-sample jump during a full-range sweep in 50 ms (host blocks of 512) must stay within 1.5 times the largest jump during a sweep so slow that it is as good as standing still. A level stepped once per block fails this measure, which the test confirms first.
- **Benchmark rows**: `chord8` now includes its filter; `chord8_mod` adds a per-voice LFO, an envelope and velocity on the cutoff and a macro (same budget); `full8` and `full8_draft` are the plan's heaviest row without the six effects, which arrive in milestone 4. The baseline was rewritten for the new rows.
- **Three older tests were made explicit rather than loosened**: the amp-envelope timing test now sets straight segments (the defaults are curved and the curves are audible now), the master-volume test allows the 5 ms glide (there within 15 ms, was one control interval), and the sends test checks that a filter at 0 % mix is transparent where it used to check that a placeholder filter did nothing.
- **Test housekeeping**: the oscillator pitch tests (offsets, master tune, reference pitch) are in `test_pitch.cpp` and `test_tuning.cpp` holds the tuning-file tests; plug-in tests that build the starter content now use a temporary folder per process, because two of them shared one and collided when run in parallel.

## Known issues
- **The machine's speed varies.** It is a virtual machine and has been seen at half speed for hours, which makes `scripts/bench.ps1` fail. If a benchmark fails unexpectedly, check that `idle` is near its baseline before looking for a regression.
- `chord8` has about a quarter of its budget to spare (3.09 % of 4 %), and `one_voice` is 14 % above its milestone 2 figure; milestone 4 must watch both.
- **Audio-rate sources act per sample only on filter cutoffs.** An oscillator's audio output or an audio-rate LFO routed anywhere else is read once per control tick (3 kHz at 48 kHz), which is too slow to be useful above a few hundred hertz. At most four such routes per filter are taken per sample; further ones fall back to the control rate.
- **Unison index** reaches only an oscillator's position, warp amounts, pan, fine and coarse; on any other destination it reads as zero.
- **MPE**: a note-off is matched by key, not by channel, so the same key held on two member channels releases the wrong one first; only the lower zone is handled.
- **The side-chain input of the envelope follower** exists in the engine (`ProcessArgs`) but the plug-in has no side-chain bus yet, so in a host the follower's side-chain source hears silence.
- **The tuning lock is not saved anywhere**: it is an engine setting until the settings file exists.
- **Routes onto a modulator's own controls and onto other routes' amounts act one control interval late** (0.33 ms at 48 kHz).
- **Warmth is not oversampled**; at high levels and high notes its harmonics can alias. The filter response display shows the linear part of the types that have no linear response (ring modulator, sample-and-hold, grit comb).
- **The defaults for decay and release curves (0.5) are audible now**, so the init patch decays faster at first than it did when curves were ignored.
- Effects, the limiter, the DC filter and the Play page parameters have no behaviour yet (milestone 4).
- The Texture player reads its sample with plain interpolation and no band-limited copies, so a sample played far above its recorded pitch can alias.
- File paths with characters outside the system's code page are not handled (table, texture, tuning and content files).
- Hard sync and other phase warps rely on oversampling alone; their aliasing falls with the quality mode but is audible in Draft at high notes.
- The CLAP build is not validated: clap-validator is pinned but not installed (required from milestone 4).
- Nobody has listened to any of it: the test patches were judged by measurements of their renders, and the standalone was not opened in this milestone.
- The Debug suite takes about three and a half minutes; Release takes about half a minute.
- FL Studio per-user plug-in folder scanning unconfirmed (no host has loaded the plug-in).
- Inno Setup not installed (needed by milestone 7). JUCE licence choice is open.

## Next
- Milestone 4 (effects and play): the three-lane rack and all thirteen effects, output protection and the DC filter, the arpeggiator, chord tools, clip player and MIDI out, and an optimization pass; the heaviest benchmark row gets its six effects.
