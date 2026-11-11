# Architecture

How the pieces fit as of milestone 1, and the rules every later milestone keeps.

## Targets

| Target | Folder | What it is | Depends on |
|---|---|---|---|
| `zederia_core` | `core/` | Static library: parameter registry, patch model, engine, offline renderer, benchmarks. No interface code and no plug-in framework | nlohmann/json |
| `Zederia` | `plugin/` | The plug-in (VST3, Standalone, CLAP): processor and a temporary editor | `zederia_core`, the plug-in framework |
| `zederia-cli` | `cli/` | `render`, `validate-patch`, `params`, `bench` | `zederia_core` |
| `zederia-mcp` | `mcp/` | The agent server; prints its version until milestone 6 | `zederia_core` |
| `zederia_tests`, `zederia_plugin_tests` | `tests/` | Core tests; processor and editor tests | the above, Catch2 |

Everything that makes or describes sound lives in `zederia_core`, so the plug-in, the command-line tool and the agent server produce identical results. The core stays portable: the only platform-specific code is `rt/Denormals.h`. `link/` (the local pipe protocol) and `content/` (recipes) are added by the milestones that need them.

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
- **Patch** (`patch/`). Parameter values in registry order plus what is not a parameter: meta text, table and sample references, modulation routes, rack layout, macro names. Values that belong to something the patch does not have (an empty slot, a missing route) stay at their defaults.
- **PatchIO.** `loadPatch` never throws and never crashes; it returns the patch plus issues, each with a code, a JSON-pointer path, a message and a fix. Errors fail the load; warnings (unknown fields, clamped values) do not. `savePatch` writes every value by default so a later change of defaults cannot alter saved sounds; `compact` leaves defaults out. Save, load, save is byte-identical. `migratePatch` brings older format versions forward one step at a time.
- **Engine** (`engine/`). `process()` splits each block at MIDI event offsets and at control ticks. Ticks fall every 16 samples on a grid fixed in absolute time; between ticks, amplitude and pitch move in straight lines toward the value computed for the next tick. Because nothing depends on where block boundaries fall, the output is bit-identical for any block size (tested). The control tick is where milestone 3 evaluates envelopes, LFOs and the modulation matrix.
- **VoiceAllocator.** Pure bookkeeping, tested on its own: poly, mono and legato, note priority, sustain pedal, stealing (oldest, quietest, releasing first). It returns actions (start, retrigger, glide, release, kill) that the engine carries out. A stolen voice fades out on a spare slot while its replacement starts.
- **OfflineRenderer** (`render/`). Patch plus events (from a note list or a Standard MIDI File) to audio, then WAV. Same engine code, fixed seed, bit-identical between runs of the same build.

## The plug-in

`ZederiaProcessor` owns one `Engine`. Each automatable registry entry becomes a `ZederiaParameter` whose value lives in the engine's `ParamStore`; a rack slot's generic parameters take the loaded effect's name and range. The non-parameter part of the patch is kept in a document guarded by a lock the audio thread never takes. State in a host project is a 12-byte header (`ZEDS`, version, length) followed by the deflated patch JSON; unreadable state is ignored and leaves the current state in place. The on-screen keyboard reaches the audio thread through a lock-free queue.

## Threads and real-time rules

| Thread | Does | Must never |
|---|---|---|
| Audio | `Engine::process`: applies changed parameters at block start, renders | Allocate, free, lock, wait, log, touch files |
| Message | Interface, patch load and save, host notifications | Block on the audio thread |
| Workers (from milestone 2) | Table builds, imports, analysis, offline renders | Touch live engine state directly |
| Link (milestone 6) | Pipe I/O for agent commands | Apply edits directly |

How the rules are kept:

- **Parameter values** cross threads through `ParamStore`: one atomic per value plus a dirty bitmap. Any thread sets; the audio thread drains at block start. No queue can overflow and nothing waits.
- **Structure** (tables, shapes, samples, rack layout) will be built off the audio thread and swapped in by pointer, with the old object retired on a non-audio thread. Milestone 1 has no such data in the engine yet; milestone 2 introduces the swap with the first wavetables.
- **Locks.** Core code may lock only through `zed::rt::Mutex`; a test scans `core/` for raw mutexes.
- **The audio-thread guard.** `process()` (and the plug-in's `processBlock`) open an `rt::AudioThreadScope`. The test programs replace the global allocation functions and report any allocation or free made inside the scope; `rt::Mutex` reports locks. After every test case the count must be zero or the run fails, so the whole test corpus is checked, not only the tests written for it.
- **Denormals.** Flush-to-zero and denormals-are-zero are set for the duration of the callback.

## Determinism

Offline renders are reproducible because: all randomness comes from one seeded generator consumed in event order; the control grid is fixed in absolute time; parameter changes apply at defined points; and no code path depends on wall-clock time or thread timing.

## Adding things

- A parameter: append a declaration after the last module in `Registry::Registry()`, append its id to `tests/data/registry_ids.txt`, run `zederia-cli params --format md --out docs/PARAMETERS.md`.
- A patch format change that old files cannot be read through: raise `kPatchVersion` and add a `MigrationStep`.
- A benchmark scenario: add it in `core/src/bench/Bench.cpp`, then `scripts/bench.ps1 -UpdateBaseline`.
