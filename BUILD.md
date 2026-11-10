# Zederia — Build Prompts

Eight prompts build Zederia from an empty folder: Prompt 0 checks and prepares the machine, Prompts 1–7 build one milestone each. The specification they build against is `zederia_build_plan.md` in this folder.

## How to run

1. Open a new session in this project folder for each prompt. One prompt per session keeps context fresh; progress is carried between sessions by files in the repository, mainly `docs/STATUS.md`.
2. Select the model **Fable 5.2** and set the session's reasoning effort to the level in the table below before sending the prompt.
3. Paste the whole prompt block. Let it run to its "Done when" line.
4. Read `docs/STATUS.md` when it finishes. Move to the next prompt only when the milestone's "Done when" conditions are reported as met with real measurements.
5. If a session stops early, start a new one and send the **Resume** prompt at the end of this file.

## Effort per prompt

| Prompt | Milestone | Fable 5.2 effort | Why this level |
|---|---|---|---|
| 0 | Environment check and setup | **low** | Mechanical checks and installs with clear pass/fail. Raise to medium only if an install misbehaves |
| 1 | Foundation | **high** | Architecture choices made here are inherited by everything else |
| 2 | Sound sources and wavetable toolkit | **xhigh** | Numerically demanding DSP with measurable quality and CPU targets |
| 3 | Filters, modulation, voicing | **xhigh** | Stability-critical filters and a large modulation system that must stay real-time safe |
| 4 | Effects, play features, optimization | **high** | Broad but well-understood work; the optimization pass needs care, not deep novelty |
| 5 | Interface | **high** | The largest volume of code; needs consistent judgment more than deep reasoning. Run a second pass at xhigh if the first result feels rough |
| 6 | Agent bridge | **xhigh** | Protocol correctness across revisions, security boundaries and cross-process design |
| 7 | Factory content and release | **high** | Sound-design iteration, final hardening and packaging |

`max` is not needed for any milestone. Reserve it for a single stubborn defect that survives a session at xhigh.

## Ground rules (every prompt relies on these)

1. **Read first.** This section, `zederia_build_plan.md`, `docs/STATUS.md` and `docs/ENVIRONMENT.md` (once they exist).
2. **The plan is the specification.** If you must deviate, do the sensible thing, and record what and why under "Decisions" in `docs/STATUS.md`.
3. **Stay inside the milestone.** Add interfaces later milestones need, but do not build their features early.
4. **Real-time rules are absolute** (plan §5.3): nothing on the audio thread may allocate, lock, wait, log or touch files.
5. **Original work only.** All code, interface artwork, wavetables, textures, presets and text are written for Zederia. Use only the dependencies listed in plan §5.1. Do not bundle third-party assets and do not imitate the layout, artwork or factory content of any existing product.
6. **Look things up.** For framework APIs, plug-in format details and the agent protocol, consult current official documentation instead of relying on memory.
7. **Verify, do not assume.** Build, run the tests, run the benchmarks and the validator, and report what you observed, including failures. Never weaken or special-case a test to make it pass.
8. **Tests ship with the code** they cover, in the same milestone.
9. **Commit as you go** on a branch named `m<N>-<topic>`, merge to `main` when the milestone is done and tag `m<N>-done`. Do not push anywhere.
10. **Ask only when blocked on something that is mine to decide** (installing software, licences, spending money, deleting work). Otherwise choose, record the choice, and continue.
11. **Finish by updating `docs/STATUS.md`**: Done · Measurements · Decisions · Known issues · Next. Then give me a short summary.

---

## Prompt 0 — Environment check and setup

**Fable 5.2 effort: low**

```text
You are preparing this Windows machine to build Zederia, a C++20 wavetable synthesizer plug-in (VST3, CLAP, Standalone) with companion command-line tools. Nothing is built yet. This session only verifies and prepares the environment so later sessions can build without surprises. Do not start building Zederia itself.

Read BUILD.md ("Ground rules") and zederia_build_plan.md sections 5.1, 5.2 and 5.6.

Do this in order:

1. Inventory. Report Windows version, CPU model and whether it supports AVX2, RAM, free space on this drive (20 GB or more is needed), and the default audio output device.

2. Check every required tool and print the version found or MISSING:
   - Git 2.40 or newer
   - A Microsoft C++ toolset v143 or newer with a Windows 10/11 SDK. Locate it with vswhere and prove it by compiling and running a small C++20 program with cl.
   - CMake 3.25 or newer
   - Ninja
   Optional, report only: clang-cl, Node.js, Inno Setup, any plug-in hosts or DAWs installed, and the agent clients claude, codex and agy.

3. If anything required is missing, stop, show me the exact install command for each item (prefer winget), and wait for my go-ahead before installing or downloading anything. After installing, re-run the checks.

4. Pin dependency versions. Look up the newest stable release of each (do not rely on memory): the JUCE framework, Catch2, nlohmann/json, cpp-httplib, pluginval, clap-validator, and the CLAP route (native framework support if the pinned release has it, otherwise the community extensions). Write them to cmake/Versions.cmake and docs/ENVIRONMENT.md. In ENVIRONMENT.md also summarize the framework's current licence options in a few lines so I can decide later.

5. Smoke test, in build/_smoke (ignored by git): fetch the pinned framework through CMake and build (a) a minimal plug-in that outputs a sine tone as VST3 and Standalone in Release, (b) a C++20 console program, (c) one Catch2 test that runs. Report configure and build times.

6. Download pluginval (ask me first, stating file name, source and size) and run it on the smoke VST3 at strictness 5 to prove validation works here.

7. Decide development install locations that need no administrator rights: the per-user VST3 and CLAP folders and a per-user tools folder. Record them in ENVIRONMENT.md and note whether my hosts scan the per-user plug-in folders by default.

8. Repository hygiene: git init with branch main, .gitignore, .gitattributes, .editorconfig, .clang-format, scripts/check-env.ps1 (a re-runnable version of steps 1 and 2 that exits non-zero if a required tool is missing), docs/ENVIRONMENT.md, and docs/STATUS.md with the headings Done, Measurements, Decisions, Known issues, Next. Commit as "m0: environment verified" and tag m0-done.

Done when: scripts/check-env.ps1 exits 0, the three smoke targets built, the validator ran on the smoke plug-in, versions are pinned, and docs/STATUS.md states what is ready and anything I still need to install or decide.
```

---

## Prompt 1 — Foundation

**Fable 5.2 effort: high**

```text
Milestone 1 of 7 for Zederia: the foundation every later milestone builds on. Follow the Ground rules in BUILD.md. Read all of zederia_build_plan.md (work from sections 5 and 6; read sections 3 and 4 so the skeleton fits what is coming), docs/ENVIRONMENT.md and docs/STATUS.md.

Build:

1. Repository and CMake as in plan 5.2: presets for Debug, Release and RelWithDebInfo; dependencies fetched at the pinned versions; warnings as errors for our own code; scripts/build.ps1, scripts/test.ps1, scripts/bench.ps1 and scripts/install-dev.ps1 (copies the plug-in to the per-user plug-in folders).

2. zederia_core:
   - The parameter registry (plan 5.5 and Appendix A). Declare every parameter that milestones 2 to 4 will need, with range, default, unit, skew, smoothing class, automatable flag and description. Behaviour comes later; ids are fixed now.
   - The patch model with JSON load, save, validation and migration (plan 4.5 and 5.6). Validation errors carry a JSON-pointer path and a plain-language fix.
   - Engine skeleton: voice allocator (poly, mono, legato, stealing), sample-accurate MIDI, the control-rate modulation tick, and a placeholder band-limited saw through an amp envelope to stereo out.
   - Offline renderer: patch plus a note list or MIDI file to WAV, deterministic with a fixed seed.

3. The plug-in target "Zederia" (VST3 and Standalone, plus CLAP by the route recorded in ENVIRONMENT.md): the processor wraps the engine, exposes the automatable parameters with stable ids, saves and restores state, and reports latency and tail. Give it a temporary generic editor (parameter list, keyboard, level meter); the real interface is milestone 5.

4. zederia-cli with subcommands render, validate-patch, params (dump the registry as Markdown and JSON) and bench. Add zederia-mcp as a target that builds and prints its version; its features are milestone 6.

5. Test and benchmark harness (plan 6.1): registry integrity, patch round-trip identity, malformed-patch fuzzing, render determinism, tuning accuracy of the placeholder oscillator, and the audio-thread guard that fails a test on any allocation or lock inside the audio callback. The bench command prints the realtime factor for a named scenario and compares it with a stored baseline.

6. Docs: docs/ARCHITECTURE.md (short: how the pieces fit, the threading rules) and a generated docs/PARAMETERS.md.

Done when: a configure and build from an empty build folder succeeds in Debug and Release; all tests pass; pluginval at strictness 10 passes on the VST3; the standalone plays notes from the on-screen keyboard; zederia-cli render produces the same file hash on two runs; docs/STATUS.md is updated with measurements. Merge, tag m1-done.
```

---

## Prompt 2 — Sound sources and wavetable toolkit

**Fable 5.2 effort: xhigh**

```text
Milestone 2 of 7 for Zederia: everything that makes sound before the filters, and the wavetable toolkit. Follow the Ground rules in BUILD.md. Read zederia_build_plan.md sections 3.1 to 3.4, 3.10, 5.3, 5.4, 5.7, 5.8, 6.1 and Appendix B, then docs/STATUS.md and docs/ARCHITECTURE.md.

Build in zederia_core, replacing the placeholder oscillator:

1. Wavetable data model: frames of 2048 samples, 1 to 256 frames, key frames plus morph mode, generated positions, the "Stepped" flag, WAV import and export with the Zederia metadata chunk, and safe hot-swapping of tables while notes play.

2. The wavetable oscillator: band-limited playback meeting the aliasing and tuning targets in plan 5.7, continuous smoothed frame morphing, all warp modes in plan 3.3 with two warp slots in series, phase and phase-random controls including "continue" and consistent start phases, fixed-pitch mode.

3. Cross-modulation between sources (phase, amplitude and ring modulation from any source, free ratios, defined evaluation order, one-sample delay for loops).

4. Unison as in plan 3.4, processed in SIMD lanes with a scalar reference path that tests compare against.

5. The Classic engine, the Sub oscillator and the Texture sample player, with per-source sends (filter balance and direct amount) feeding placeholder pass-through filter slots.

6. Quality modes for the source section (Draft, Standard, High, Ultra) with low-latency oversampling for warped and modulated paths.

7. The wavetable toolkit as plain library functions, with no interface code: every process, morph, sort and frame operation in plan 3.10, the formula language of Appendix B (parser, bytecode, limits, clear error messages), all import methods (single cycles, pitch-tracked and fixed-length slicing, spectral snapshots, image, stereo options) and the resynthesis operations. Expose them through zederia-cli table subcommands.

8. A starter set of about 12 original wavetables and 6 textures generated from recipes in content/, enough to exercise every code path. The full factory set is milestone 7.

Test and measure (plan 6.1): tuning, aliasing thresholds per quality mode, the click test on position sweeps, unison level compensation, SIMD against scalar, each toolkit operation on known inputs, formula parsing and error cases, import on synthetic signals of known pitch. Add benchmark scenarios for the first three rows of plan 5.8 and store baselines. If this machine cannot reasonably meet a budget, recalibrate once now and record the reason.

Done when: all tests pass including the audio-thread guard; the stated aliasing and tuning targets are met and the measured numbers are in docs/STATUS.md; the benchmark budgets are met; pluginval strictness 10 still passes; the standalone plays all three oscillators, Sub and Texture from the generic editor. Merge, tag m2-done.
```

---

## Prompt 3 — Filters, modulation and voicing

**Fable 5.2 effort: xhigh**

```text
Milestone 3 of 7 for Zederia: shaping and motion. Follow the Ground rules in BUILD.md. Read zederia_build_plan.md sections 3.5 to 3.7, 5.3 to 5.5, 5.7, 5.8 and 6.1, then docs/STATUS.md and docs/ARCHITECTURE.md.

Build in zederia_core:

1. The two voice filter slots with every family and type in plan 3.5, the Character control per type, stereo offset, key tracking, Mix/Level switch, and routing: the serial-to-parallel blend plus the per-source sends from milestone 2. Use zero-delay-feedback forms; oversample nonlinear types according to quality. Provide the data the response display will need (magnitude and phase at the current, modulated settings).

2. Envelopes: four, with delay, attack, hold, decay, sustain, release and per-segment curves; tempo-linked times; multi-segment mode with sustain point and loop region for envelopes 2 to 4.

3. LFOs: eight, with the shape model (points, curves), all modes, free and synced rates including audio-rate mode, anchor to song position, phase, delay, rise, smooth, polarity, per-voice or global, key-tracked rate. Flux generators: two, four types.

4. Macros (eight) and every other source listed in plan 3.7, including MPE, polyphonic aftertouch, learned MIDI CCs, audio-rate source outputs and the envelope follower.

5. The modulation matrix: 64 routes with Via, amount, polarity, curve (including drawn curves), enable, mute and solo; polyphonic where both ends are per-voice; any continuous parameter as a destination, including modulator rates and other routes' amounts. Keep it real-time safe and cheap when routes are unused.

6. Voice features in plan 3.6: glide (time, curve, always or legato-only, scaled), bend ranges, MPE handling, master tuning and reference pitch, Scala and .tun loading with tuning lock, and the Character controls (drift, variance, warmth, noise floor). Parameter smoothing for every continuous control (plan 5.7).

Test and measure (plan 6.1): linear filter responses against analytic curves, ten-second stability sweeps at maximum resonance with audio-rate cutoff modulation for every type, self-oscillation pitch, envelope timing, LFO sync phase at bar lines, matrix arithmetic with Via, curves and polarity, MPE routing, tuning files, zipper-noise checks. Extend the benchmarks to the 8-voice rows of plan 5.8.

Done when: all tests pass including the audio-thread guard and stability sweeps; budgets are met with measurements in docs/STATUS.md; pluginval strictness 10 passes; a handful of hand-written test patches (bass, pad, pluck, lead) load from JSON and render sensibly through zederia-cli. Merge, tag m3-done.
```

---

## Prompt 4 — Effects, play features and optimization

**Fable 5.2 effort: high**

```text
Milestone 4 of 7 for Zederia: the effects rack, the play features, and the performance pass that completes the engine. Follow the Ground rules in BUILD.md. Read zederia_build_plan.md sections 3.8, 3.9, 3.12, 5.3 to 5.8 and 6.1, then docs/STATUS.md and docs/ARCHITECTURE.md.

Build in zederia_core, with the plug-in processor updated to match:

1. The effects rack: lanes A, B and Master with 8 slots each, lane B parallel or in series, sends from the filter outputs and the direct signal, any effect in any slot any number of times, click-free insert, remove, reorder and bypass while audio runs, per-slot mix and power, idle detection after tails, the global FX off switch, rack lock, and rack and slot presets. Map slots to the generic automatable parameters described in plan 5.5.

2. Every effect in the table in plan 3.8 with all listed modes and controls. Give the reverb and the chorus particular attention; plan 5.7 states the reverb requirements. Effect parameters are modulation destinations.

3. Play features (plan 3.9): the arpeggiator with step lane and pattern slots, chord memory, scale lock and strum, the clip player with MIDI file import and export, MIDI output of generated notes, MIDI thru, MIDI learn with savable CC maps, and program change.

4. Global quality modes across the whole engine, separate realtime and offline settings, automatic offline quality when the host renders faster than real time, correct latency reporting, the output limiter and DC filter.

5. Optimization pass: profile the scenarios in plan 5.8, remove the largest costs, confirm inactive modules cost nothing and idle instances meet the idle budget, and check there are no denormal slowdowns on decaying tails.

Test and measure (plan 6.1): per-effect level and tail expectations, idle detection, limiter ceiling, arpeggiator order and timing for every mode, clip playback against its source MIDI file, MIDI output content, quality-mode switching without clicks, golden-audio renders for a set of reference patches, and all benchmark rows in plan 5.8.

Done when: all tests pass including the audio-thread guard; every budget in plan 5.8 except the interface rows is met with numbers in docs/STATUS.md; pluginval strictness 10 passes; clap-validator passes if the CLAP target exists; zederia-cli renders a full patch with effects and an arpeggio deterministically. Merge, tag m4-done.
```

---

## Prompt 5 — Interface

**Fable 5.2 effort: high**

```text
Milestone 5 of 7 for Zederia: the complete interface, replacing the temporary generic editor. Follow the Ground rules in BUILD.md. Read zederia_build_plan.md sections 2, 3 (all of it, with 3.7, 3.10, 3.11 and 3.13 closely), 5.3, 5.5, 5.8 and 6.1, then docs/STATUS.md and docs/ARCHITECTURE.md. Section 2 explains what users struggle with in instruments of this kind; let it guide interaction decisions the plan does not spell out.

Work in this order:

1. Component kit and theme tokens first: knob, slider, number box with unit-aware typing, switch, menu, tab, card, drag chip, tooltip and help strip, curve editor (shared by LFO, multi-segment envelope, warp curve and drive curve), meters, and the display components (3D and 2D wavetable, filter response, envelope, spectrum, signal map). Vector drawing only, free scaling from 70% to 300%, the Graphite, Paper and Contrast themes. The look must be Zederia's own.

2. A snapshot tool before any page: a zederia-cli ui-snapshot command (or test) that renders any page offscreen to PNG at a chosen scale. Use it throughout to look at your own work, and add the automatic checks from plan 6.1 (clipping, overlap, truncated text).

3. Pages: SYNTH (source cards, Stage, filters and routing, voice, modulator dock, keyboard), FX (three lanes, drag between lanes), MOD (route list with search, sort, bulk source replacement, and the mod map overlay), TABLE (the full workshop of plan 3.10, with the import wizard), PLAY (arpeggiator, chords, clip player with piano-roll view), LIBRARY (facets, search, sortable results, audition, bulk edit, pack import, module presets), and an AGENT page shell that milestone 6 will fill.

4. Modulation interaction exactly as in plan 3.7: drag to assign, depth arcs, live value markers, hover lists with inline amounts, alt-drag depth, polarity flip, mute, solo and remove.

5. Undo and redo with a named history list covering parameters, routing, shapes, wavetable edits and preset loads. A/B compare. Locks. Randomize, Mutate and Blend. Copy and paste for every module and shape.

6. Top bar, command palette, context menus, unit-aware value entry, Simple view, preferences, and accessibility (keyboard access to every control, names and values exposed to screen readers).

7. Interface performance (plan 3.13 and 5.8): frame cap, no animation when hidden, no GPU requirement, workshop interaction within budget.

Test and measure: snapshot checks for every page at 100%, 150% and 200%; a test that every registry parameter is reachable from the interface; undo and redo round-trips; interface CPU and workshop latency against plan 5.8. Inspect the snapshots yourself and fix what looks wrong: crowding, misalignment, unlabeled controls, poor contrast.

Done when: all automated checks pass; every feature of milestones 2 to 4 can be driven from the interface; state survives closing and reopening the editor and the host project; pluginval strictness 10 passes with the editor; docs/STATUS.md lists measurements and includes the snapshot paths for me to review. Merge, tag m5-done.
```

---

## Prompt 6 — Agent bridge

**Fable 5.2 effort: xhigh**

```text
Milestone 6 of 7 for Zederia: the bridge that lets AI agents work on the instrument. Follow the Ground rules in BUILD.md. Read zederia_build_plan.md section 4 in full, plus 5.3, 5.6, 6.1 and Appendix C, then docs/STATUS.md and docs/ARCHITECTURE.md.

Before writing code, read the current Model Context Protocol specification and its changelog from the official site, and check which protocol revisions the installed clients on this machine speak (see docs/ENVIRONMENT.md). Where the specification differs from the plan's summary, the specification wins; record the difference.

Build:

1. zederia-mcp: stdio transport, and Streamable HTTP on 127.0.0.1 behind a flag with origin validation and a bearer token. Support both protocol generations described in plan 4.3. Keep all protocol code in this executable.

2. Headless sessions on top of zederia_core, addressed by explicit target handles, and the live link: a per-user local pipe from each plug-in instance with Agent Link enabled, a discovery file, a random token, commands queued and applied at block boundaries, and offline renders performed on a copy of the instance's state off the audio thread.

3. Every tool in plan 4.4 with input and output schemas, structured results, annotations, progress and cancellation for long renders, and validation errors that tell an agent exactly what to fix. Add the resources and prompts listed there.

4. The analysis module (Appendix C), compare_audio, and the optional spectrogram image.

5. Music tools (plan 4.7): theory helpers, clip validation, loop rendering with the three tail options, MIDI export, pack export with the naming rule, one-shots and multi-sampled notes. Agent-written clips load into the clip player.

6. Safety exactly as in plan 4.8: permissions, the file sandbox, limits, undo labelling per client, and "Revert agent session".

7. The AGENT page in the plug-in: link switch, connected clients, activity log, permissions, revert, and copy buttons for the client configuration snippets in plan 4.9.

8. docs/AGENT_GUIDE.md, served as the zederia://guide/agent resource: parameter semantics, sound-design recipes, the design loop, loudness and naming conventions, and worked examples.

Test (plan 6.1): scripted protocol sessions for each generation and transport; schema validation of every tool's inputs and outputs; error paths; sandbox escapes rejected; token and origin checks; cancellation; live-link tests against a running standalone instance; the audio-thread guard while agents edit during playback.

Then prove it with a real client installed on this machine: register the server, and from that client carry out example sessions 1 and 3 in plan 4.10 (design and save a preset from a brief; write a chord progression and a melody and render both as loops with MIDI files). If no client can be driven from this session, say so and give me the exact steps to run it myself.

Done when: conformance and safety tests pass; the example sessions complete and their output files exist; edits from an agent appear in a live instance and can be undone; docs/STATUS.md records the protocol revisions supported and the clients verified. Merge, tag m6-done.
```

---

## Prompt 7 — Factory content and release

**Fable 5.2 effort: high**

```text
Milestone 7 of 7 for Zederia: factory content, hardening and a release candidate. Follow the Ground rules in BUILD.md. Read zederia_build_plan.md sections 2, 6, 7, 8 and 9, then docs/STATUS.md and docs/AGENT_GUIDE.md.

1. Factory content, all original and generated or designed here (plan 7 and 9):
   - About 60 wavetables and 20 textures from recipes in content/, covering basic shapes, harmonic series, formant and vocal-like sets, metallic and inharmonic sets, sweeps and evolving motion. Build them with zederia-cli so the origin of each is reproducible.
   - About 120 presets spread across basses, leads, plucks, keys, pads, strings and brass-like sounds, sequences, effects and percussion. Design them through the agent tools from milestone 6: brief, edit, render, analysis, adjust, save. Each preset must have sensible macros, tags, a consistent loudness (see the agent guide), and no clipping. Aim for range and musical usefulness, and include warm and organic sounds as well as bright digital ones.
   - 24 LFO shapes, 16 clips, a default preset, and effect rack presets.
   - A content check command that loads and renders every preset and fails on errors, clipping, silence or CPU above budget.

2. Hardening: run the full test suite, the benchmarks, pluginval at strictness 10 and clap-validator; fix what they find. Run long-duration soak renders with random parameter automation and preset switching to catch crashes, stuck notes, runaway levels and memory growth. Review plan section 8 and confirm each mitigation is in place.

3. Host checks: with the standalone and the hosts listed in docs/ENVIRONMENT.md, verify loading, playing, automation, project save and reopen, offline render and several instances. Automate what can be automated and give me a short checklist for the parts that need my hands and ears.

4. Packaging: a per-user installer that needs no administrator rights (Inno Setup if available, otherwise a PowerShell install and uninstall pair) placing the plug-ins, tools and factory content in the locations in plan 5.6; a NOTICE file listing dependency licences; version stamping in the binaries.

5. Documentation: docs/MANUAL.md (a user manual organized like the interface, with the help-strip texts as its source where possible), an updated agent guide with a cookbook of example sessions, and docs/RELEASE_NOTES.md.

Done when: the content check passes for every preset; all tests, benchmarks and validators pass; the installer installs to an empty user profile location and uninstalls without leftovers; docs/STATUS.md contains the final measurements, the host checklist with what you verified and what remains for me, and an honest list of known issues. Merge, tag v1.0.0-rc1.
```

---

## Helper prompts

### Resume (same effort as the interrupted milestone)

```text
A previous session on a Zederia milestone ended before its "Done when" conditions were met. Follow the Ground rules in BUILD.md. Read docs/STATUS.md, the git log and the matching prompt in BUILD.md, work out what is finished and what is not by building and running the tests (do not trust notes alone), then continue that milestone to completion and update docs/STATUS.md.
```

### Milestone audit (effort: medium)

```text
Audit the most recently tagged Zederia milestone against zederia_build_plan.md and its prompt in BUILD.md. Build from an empty build folder, run every test, benchmark and validator, and compare what exists with what was required. Change no code. Report: requirements met, requirements missing or partial, measurements against budgets, and risks for the next milestone. Write the report to docs/audits/m<N>.md.
```
