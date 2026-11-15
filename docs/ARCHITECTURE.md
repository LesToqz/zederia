# Architecture

How the pieces fit as of milestone 3, and the rules every later milestone keeps.

## Targets

| Target | Folder | What it is | Depends on |
|---|---|---|---|
| `zederia_core` | `core/` | Static library: parameter registry, patch model, engine (sound sources, filters, modulators, the matrix), tuning files, wavetable model and toolkit, content recipes, offline renderer, benchmarks. No interface code and no plug-in framework | nlohmann/json |
| `Zederia` | `plugin/` | The plug-in (VST3, Standalone, CLAP): processor and a temporary editor | `zederia_core`, the plug-in framework |
| `zederia-cli` | `cli/` | `render`, `validate-patch`, `params`, `bench`, `table ...`, `texture build`, `content build` | `zederia_core` |
| `zederia-mcp` | `mcp/` | The agent server; prints its version until milestone 6 | `zederia_core` |
| `zederia_tests`, `zederia_plugin_tests` | `tests/` | Core tests; processor and editor tests | the above, Catch2 |

Everything that makes or describes sound lives in `zederia_core`, so the plug-in, the command-line tool and the agent server produce identical results. The core stays portable: the only platform-specific code is `rt/Denormals.h` and the SIMD wrappers in `dsp/Simd.h`. `link/` (the local pipe protocol) is added by the milestone that needs it. The repository's `content/` folder holds recipes (JSON), never audio: the factory wavetables and textures are built from them.

## The core

```
 Registry ──► Patch ◄──► PatchIO (JSON)
    │           │
    │           ▼ loadPatch()
    │        ParamStore  ◄── any thread sets values (host, interface, agent link)
    │           │ drained at the start of each block
    ▼           ▼
          Engine::process ──► VoiceAllocator ──► voices ──► stereo out
                 ▲
     MidiEvent (sample offsets)        OfflineRenderer drives the same Engine block by block
```

- **Registry** (`params/`). Every parameter is declared once in `Registry.cpp`: id, range, default, unit, skew, smoothing class, automatable and modulatable flags, scope, description, and the JSON pointer of its value in a patch. Host parameters, the patch schema, `docs/PARAMETERS.md` and the JSON schema dump are generated from it. It is append-only: `tests/data/registry_ids.txt` records the order and a test fails if an existing id moves or disappears. The host's numeric parameter id is a hash of the string id (`hostIdFor`), checked for collisions.
- **Effect types and slots.** Each rack slot has `on`, `mix` and sixteen generic parameters `p1..p16`. An effect type lists its own parameters in a fixed order; parameter k of the loaded effect is the slot's `p(k+1)`. Values are stored in the effect parameter's own unit, and `Patch::effectiveDef()` returns the definition that governs a value. In a patch the effect's parameters appear under their own names.
- **Patch** (`patch/`). Parameter values in registry order plus what is not a parameter: meta text, table and sample references, modulation routes (with drawn curves), LFO shapes, multi-segment envelope shapes, tempo-linked envelope times, warp curves, a tuning, rack layout, macro names. Values that belong to something the patch does not have (an empty slot, a missing route) stay at their defaults.
- **PatchIO.** `loadPatch` never throws and never crashes; it returns the patch plus issues, each with a code, a JSON-pointer path, a message and a fix. Errors fail the load; warnings (unknown fields, clamped values) do not. `savePatch` writes every value by default so a later change of defaults cannot alter saved sounds; `compact` leaves defaults out. Save, load, save is byte-identical. `migratePatch` brings older format versions forward one step at a time.
- **Engine** (`engine/`). `process()` splits each block at MIDI event offsets and at control ticks. Ticks fall every 16 samples on a grid fixed in absolute time; between ticks, amplitude and pitch move in straight lines toward the value computed for the next tick. Because nothing depends on where block boundaries fall, the output is bit-identical for any block size (tested, with filters and a full matrix too). The control tick is where envelopes, LFOs and the modulation matrix are evaluated (see Shaping and motion).
- **VoiceAllocator.** Pure bookkeeping, tested on its own: poly, mono and legato, note priority, sustain pedal, stealing (oldest, quietest, releasing first). It returns actions (start, retrigger, glide, release, kill) that the engine carries out. A stolen voice fades out on a spare slot while its replacement starts.
- **OfflineRenderer** (`render/`). Patch plus events (from a note list or a Standard MIDI File) to audio, then WAV. Same engine code, fixed seed, bit-identical between runs of the same build. It renders at the patch's offline quality unless told to use the live one, and takes the tables and the texture sample the patch names.

## Sound sources

```
 Wavetable (frames a user edits) ──► TableData (what the oscillator reads) ──► rt::Handoff ──► Engine
 WavFile / Import / Toolkit / Formula / Resynth / Recipe all produce or change a Wavetable
 SampleData (texture) ───────────────────────────────────────────────────────► rt::Handoff ──► Engine

 per voice:  Sub ─┐
              Texture ─┤  (modulators first, in dependency order)
              Osc 1..3 ┴─► level, pan, sends ──► bus Filter 1 │ bus Filter 2 │ direct ──► amp envelope ──► out
```

- **Wavetable** (`table/Wavetable.h`). 1 to 256 key frames of 2048 samples, a morph mode (none, crossfade, spectral, spectral with zeroed phase, spectral with aligned fundamental), a Stepped flag and a name. Saved as a WAV file whose `ZEDT` chunk holds the metadata as JSON; files without the chunk are read by the usual conventions (a text marker chunk, then lengths that divide by 2048, 1024, 4096, 512 or 256, then one cycle of any length). Damaged files are refused with a message, never a crash.
- **TableData** (`table/TableData.h`). The read-only form the oscillator plays, built off the audio thread: the positions (the key frames themselves for none, crossfade and stepped tables, 256 generated positions for the spectral modes), each stored at 28 band limits (1024 harmonics down to 1, about three per octave). A level's size is chosen so the interpolation error of a sawtooth stays below -104 dBc, and the data is pre-emphasised for the six-point spline the oscillator reads it with, so the response is flat. Builds are cached by content hash for the life of the process.
- **Oscillator kernel** (`src/engine/OscKernel.inl`). One template, compiled three times: scalar, 4 lanes and 8 lanes. A lane is one unison voice. The three results are bit-identical (no fused multiply-add, a fixed order for summing lanes), so the scalar kernel is the reference the vector ones are tested against. Phase is a 32-bit integer; the level is chosen per lane so that no harmonic lies above half the sample rate, and a change of level is crossfaded over one control tick. Position moves between the two nearest stored positions (or jumps, for Stepped).
- **Warps.** Two slots in series, each a phase stage or an amplitude stage: sync, soft sync, bend, pulse width, skew, mirror, flip, quantize, curve, fold, drive, and phase, amplitude and ring modulation from another source. The Classic engine (saw, square, triangle and sine with a polynomial correction at the steps) takes the amplitude stages and the modulations.
- **Cross-modulation.** Sources are rendered in dependency order: Sub, Texture, then the oscillators sorted so that a modulator comes before what it modulates. A loop, or an oscillator modulating itself, runs one sample at a time with a delay of one base-rate sample.
- **Unison.** Up to 16 lanes laid out from the centre outward: detune laws, stack intervals, blend between the centre and the sides at constant power, stereo width, position and warp spread, start phases.
- **Quality.** Draft, Standard, High and Ultra oversample by 1, 2, 4 and 8 the paths that can alias (a warped or modulated oscillator, a modulator, the Classic engine) and come back down through polyphase half-band filters that add no latency. A plain wavetable oscillator is clean at the base rate in every mode and is never oversampled.
- **Sub and Texture.** Sub is one simple shape (sine, triangle, saw, square, 25 % pulse or rounded square) under the note. Texture plays a sample (one-shot or looped, keytracked or fixed, with start and random start). Each source has a level, a pan and a send to each of the two filter slots and to a direct bus.

## Shaping and motion

```
 ParamStore ─► p (as written) ─► nb (smoothed, normalized) ─► pe (plus the routes all voices share) ─► derive()
                                                    └────────► + Voice::off (per-voice routes) ─► vval() ─► this voice's values
 sources:  envelopes 1-4 │ LFOs 1-8 │ Flux 1-2 │ macros │ MIDI and per-note │ audio taps │ follower │ bar phase
 per voice:  sources ─► buses ─► Warmth, Noise floor ─► Filter 1 ─(series ↔ parallel)─► Filter 2 ─► amp, pan ─► out
```

- **Engine source files** (`core/src/engine/`). `Engine.cpp` holds the state, the block loop and the public functions; it includes `EngineTypes.inl` (what every voice shares and what each voice keeps), `EngineMod.inl` (parameters, sources, the matrix, MIDI), `EngineVoice.inl` (a voice from note-on to its control tick) and `EngineRender.inl` (the audio loops).
- **Parameters on the audio thread.** A value passes three stages: `p`, what was last written; `nb`, the same after a 5 ms glide in the parameter's own (skewed) scale, for every continuous parameter; and `pe`, `nb` plus the routes that are the same for every voice. Everything the engine derives reads `pe`, so smoothing and modulation reach every control the same way. Only parameters that are moving or modulated are touched at a tick; `derive()` runs only when one of them changed.
- **The control tick.** At each tick every modulator is moved to where it will be at the next tick, the routes are added up, and everything that sounds is pointed there in a straight line (pitch, lane values, gains, sends, filter coefficients, the amp). Nothing waits a tick and nothing steps. What the engine reports to other threads (`sourceValue`, `liveValue`, `filterView`) is therefore one control interval ahead of the audio.
- **Filters** (`engine/Filter.h`). `flt::VoiceFilter` is one slot of one voice in stereo: drive stage, core, body stage (state-variable family), dry/wet. Seventy types in sixteen families, all from zero-delay-feedback parts. Linear cores run at the base rate; the saturating stages and the ladder, howl and ring-modulator cores run oversampled by the quality mode through the same half-band filters the oscillators use. `designLinear()` describes a linear type as sections and `response()` evaluates it: the audio code and the display use the same design, so they cannot disagree. A voice redesigns a filter only when its settings changed. Ramps advance by repeated addition, never by multiplying, so a control interval cut into several calls gives the same bits as one call.
- **Routing.** Each source has shares into filter 1, filter 2 and a direct bus. `voice.filter_routing` blends from series (filter 1 into filter 2) to parallel. A filter that is off passes its input through.
- **Envelopes** (`engine/Envelope.h`). `Envelope` has delay, attack, hold, decay, sustain and release with a curve on the attack, decay and release; `MultiEnvelope` (envelopes 2 to 4) travels through up to 32 points with a sustain point and a loop. Times can be linked to the tempo (a note length instead of milliseconds); they become samples on the audio thread, where the tempo is known.
- **LFOs and Flux** (`engine/Lfo.h`). An LFO reads a 1024-point table compiled from a drawn `Shape` (`patch/Shape.h`); modes are free, retrigger, one-shot and loop-from-point, with phase, delay, rise, smooth, polarity, tempo sync, Anchor (the phase is taken from the song position), key-tracked rate, and a per-voice or shared instance. Flux is four random generators (orbit, burst, stepped, smooth). A modulator nobody listens to is not run.
- **The matrix.** `setRoutes()` resolves the routes of a patch off the audio thread; the audio thread sorts them into four kinds whenever a route or a scope setting changes. *Shared* routes (no per-voice end) add into `pe` once per tick. *Per-voice* routes (a per-voice source or Via, and a destination in a voice module) add into the offsets each voice keeps, and the voice derives its own copy of just the module they reach. *Audio-rate* routes (the audio output of a source, or an audio-rate LFO, onto a filter cutoff; up to four per filter) move the cutoff every sample. *Unison-index* routes act per lane on the position, warp amounts, pan and pitch of an oscillator. A per-voice source on a destination that exists once reads the most recent voice. The arithmetic of a route is in `engine/ModMath.h` and is shared with the tests.
- **Voice.** Glide moves the key position (time, curve, always or legato-only, per octave when scaled); the tuning turns the key position into pitch, so a glide passes through the tuned pitches. `Tuning` (`tuning/Tuning.h`) holds a pitch per key and is read from Scala `.scl`/`.kbm` or `.tun` files; a patch can carry one, and a lock keeps the current one across patch loads. MPE gives each member channel its own bend, pressure and slide. Character: drift (a slow random wander per oscillator), variance (per-note random offsets of level, pan, cutoff and envelope speed), warmth (a cubic saturation and a slight tilt before the filters) and noise floor.

## The wavetable toolkit

Library functions on a `Wavetable`, all reachable from `zederia-cli table`:

- **Toolkit** (`table/Toolkit.h`): frame operations (normalise, remove DC, mirror, fades, low-pass and more), harmonic operations (clear, fill, randomise, octave shifts, odd or even only), frame management (insert, duplicate, delete, move, reverse, reduce), morph generation between frames and sorting by brightness or level.
- **Formula** (`table/Formula.h`): a small expression language over `t`, `x`, `y`, `z`, the harmonic number, the input and the selected frame; in the wave or the harmonic domain. Expressions are compiled to bytecode with limits on length, depth and size; errors carry the column and, for a misspelt name, a suggestion.
- **Import** (`table/Import.h`): single cycles, a recording cut by tracked pitch (with drift correction) or by a fixed length, spectral snapshots, and an image read row by row. `util/Image` decodes PNG, BMP and PNM with its own inflate.
- **Resynth** (`table/Resynth.h`): bakes a warp, or a whole patch played at one note, into a new table.
- **Recipes** (`content/Recipe.h`): a table or a texture described as a list of steps in JSON. `zederia-cli content build` turns the repository's `content/` folder into the factory folder; the same recipe always gives the same bytes.
- **ContentLibrary** (`content/Library.h`): resolves the references a patch holds (`factory:Group/Name`, `user:...`, or a path) to files, lists what the folders hold, and loads what a patch names into an engine.

## The plug-in

`ZederiaProcessor` owns one `Engine`. Each automatable registry entry becomes a `ZederiaParameter` whose value lives in the engine's `ParamStore`; a rack slot's generic parameters take the loaded effect's name and range. The non-parameter part of the patch is kept in a document guarded by a lock the audio thread never takes. State in a host project is a 12-byte header (`ZEDS`, version, length) followed by the deflated patch JSON; unreadable state is ignored and leaves the current state in place. The on-screen keyboard reaches the audio thread through a lock-free queue.

The processor holds a `ContentLibrary` (factory folder, user folder). Applying a patch, or choosing a table or texture in the temporary editor, loads the file on the calling thread and hands the result to the engine; a reference that cannot be loaded leaves the built-in table or noise in place and is reported, not fatal. The reference, not the audio, is what a patch and the host state store.

## Threads and real-time rules

| Thread | Does | Must never |
|---|---|---|
| Audio | `Engine::process`: applies changed parameters at block start, renders | Allocate, free, lock, wait, log, touch files |
| Message | Interface, patch load and save, host notifications | Block on the audio thread |
| Workers (from milestone 2) | Table builds, imports, analysis, offline renders | Touch live engine state directly |
| Link (milestone 6) | Pipe I/O for agent commands | Apply edits directly |

How the rules are kept:

- **Parameter values** cross threads through `ParamStore`: one atomic per value plus a dirty bitmap. Any thread sets; the audio thread drains at block start. No queue can overflow and nothing waits.
- **Structure** (tables, samples, warp curves, routes, LFO shapes, envelope shapes, the tuning; later the rack layout) is built off the audio thread and handed over through `rt::Handoff<T>`: the newest offered object waits in an atomic slot, the audio thread takes it at the start of a block, and the object it replaces goes into a ring that a non-audio thread empties (`Engine::collectGarbage`). The audio thread never frees. A playing voice crossfades from the old table or sample to the new one over 256 samples, so a swap does not click (tested from a second thread while audio runs).
- **Locks.** Core code may lock only through `zed::rt::Mutex`; a test scans `core/` for raw mutexes.
- **The audio-thread guard.** `process()` (and the plug-in's `processBlock`) open an `rt::AudioThreadScope`. The test programs replace the global allocation functions and report any allocation or free made inside the scope; `rt::Mutex` reports locks. After every test case the count must be zero or the run fails, so the whole test corpus is checked, not only the tests written for it.
- **Denormals.** Flush-to-zero and denormals-are-zero are set for the duration of the callback.

## Determinism

Offline renders are reproducible because: all randomness comes from one seeded generator consumed in event order; the control grid is fixed in absolute time; parameter changes apply at defined points; and no code path depends on wall-clock time or thread timing.

## Adding things

- A parameter: append a declaration after the last module in `Registry::Registry()`, append its id to `tests/data/registry_ids.txt`, run `zederia-cli params --format md --out docs/PARAMETERS.md`.
- A patch format change that old files cannot be read through: raise `kPatchVersion` and add a `MigrationStep`.
- A benchmark scenario: add it in `core/src/bench/Bench.cpp`, then `scripts/bench.ps1 -UpdateBaseline`.
- A factory wavetable or texture: add a recipe under `content/wavetables` or `content/textures` (see `content/README.md`); `scripts/install-dev.ps1` rebuilds the factory folder.
- A warp mode: add the choice to the registry, its derived value in `warpDerive` and its stage in `OscKernel.inl`; the kernel test then checks all three widths against each other.
- A filter type: append its name to the filter type list in the registry, give it a `TypeInfo` in `Filter.cpp` (the two lists are checked against each other), and a design in `designLinear` or a core of its own; the filter tests then run it through the response and stability checks.
- A modulation source: append it to `declareSources()` and give it a position in `EngineTypes.inl` (the engine checks that the two agree when it is built); fill its value in `globalTick` (shared) or `evalVoice` (per voice).
