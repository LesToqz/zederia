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

## Measurements
Milestone 1, measured 2026-11-12 on the machine in `docs/ENVIRONMENT.md`.

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

Benchmarks (Release, one core, 48 kHz, 512-sample blocks, best of 5 runs of 20 s; baseline in `tests/bench/baseline.json`):

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

## Known issues
- The placeholder envelope has straight segments and ignores the curve parameters; most declared parameters have no behaviour yet (by design: milestones 2 to 4).
- The CLAP build is not validated: clap-validator is pinned but not installed (required from milestone 4).
- The standalone was checked by window capture, not by ear.
- The Debug fuzz test takes about 110 s (10 s for the whole suite in Release).
- FL Studio per-user plug-in folder scanning unconfirmed; `scripts/install-dev.ps1` has not been run.
- Inno Setup not installed (needed by milestone 7). JUCE licence choice is open.

## Next
- Milestone 2 (sound sources and wavetable toolkit). First decision there: how FFT, deflate and image decoding reach the core (see Decisions).
