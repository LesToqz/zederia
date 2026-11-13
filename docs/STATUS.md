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

## Measurements
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

## Known issues
- **The machine's speed varies.** It is a virtual machine and has been seen at half speed for hours, which makes `scripts/bench.ps1` fail (`chord8` about 5 %). If a benchmark fails unexpectedly, check that `idle` is near its baseline before looking for a regression.
- `chord8` has less than a factor of two to spare on its budget before the filter exists; milestone 3 must watch it.
- The placeholder envelope has straight segments and ignores the curve parameters; the filter slots pass audio through; modulation, effects and the remaining parameters have no behaviour yet (by design: milestones 3 and 4).
- The Texture player reads its sample with plain interpolation and no band-limited copies, so a sample played far above its recorded pitch can alias.
- File paths with characters outside the system's code page are not handled (table, texture and content files).
- Hard sync and other phase warps rely on oversampling alone; their aliasing falls with the quality mode but is audible in Draft at high notes (numbers above).
- The CLAP build is not validated: clap-validator is pinned but not installed (required from milestone 4).
- The standalone was checked by window capture, not by ear.
- The Debug suite takes about 4.5 minutes (the patch fuzz test and the audio-thread guard test dominate); Release takes about half a minute.
- FL Studio per-user plug-in folder scanning unconfirmed (the plug-in is now installed there by `scripts/install-dev.ps1`, but no host has loaded it).
- Inno Setup not installed (needed by milestone 7). JUCE licence choice is open.

## Next
- Re-run `scripts/bench.ps1` once the machine is at normal speed (see Known issues).
- Milestone 3: filters in the two slots, envelopes, LFOs, the modulation matrix and the shape model (which also gives the warp curve its patch field).
