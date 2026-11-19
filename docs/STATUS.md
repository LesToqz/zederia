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
- Milestone 4 (effects, play features and the performance pass), tagged `m4-done`:
  - The effects rack in the engine: lanes A, B and Master of eight slots; lane B beside lane A or after it; the filter 1, filter 2 and direct buses each split between A and B; any effect in any slot any number of times; per-slot power and mix; insert, remove, move (within or between lanes), power, lane B routing and FX off while audio plays, all without clicks (an effect that has to move fades out, moves while transparent and fades back in; a continuing effect keeps its tail); idle after its tail (no cost until its input returns); rack lock; rack presets (`zederia.rack`) and single-effect presets (`zederia.fx`). Slots map to the generic `fx.<lane><n>.p1..p16` parameters, whose host names follow the loaded effect; effect parameters are modulation destinations, and routes move with their effect.
  - All thirteen effects of plan 3.8 with every listed mode and control: Drive (fifteen modes including two drawn curves with a morph, pre or post morph filter, oversampled by quality), Compressor (one band with a soft knee and a limit at the top ratio, or three Linkwitz-Riley bands with upward and downward compression), EQ (four bands, response for the display), Filter (any voice filter type in stereo), Chorus (2 to 6 voices, Hz or synced, two delay times, depth, feedback, low- or high-pass on the wet signal), Flanger, Phaser (2 to 12 stages), Delay (dual, ping-pong, pre-tap; ms or synced with a fine offset whose ends are the triplet and dotted lengths; link, feedback band, saturation, wow, ducking), Reverb (plate, hall, room: a sixteen-line feedback delay network with input diffusion, moving lines, early reflections, size, decay, pre-delay, cuts, damping, width, freeze), Swarm (one to seven micro-shifted copies), Widener (four taps of the mid signal into the side; mono-compatibility meter), Utility, Limiter.
  - Output stage after the rack: master volume, a DC filter (on by default) and output protection (soft clip or a -0.3 dBFS limiter).
  - Quality: realtime and offline settings across voices and effects; offline quality when the host renders offline or faster than real time; a quality change while notes play does not click; no mode adds latency.
  - Play: the arpeggiator (up, down, up-down with and without repeated ends, random, walk, as played, chord; one to four octaves; synced rate; gate; swing; latch; restart; a step lane of up to 32 steps with on, tie, rest, velocity, transposition and ratchets; twelve pattern slots), chord memory, scale lock (fourteen scales, snap or filter), strum (up, down, alternate), the clip player (eight slots, transport or key trigger with transposition, latch, loop), clip import and export as Standard MIDI Files, MIDI out of the notes the play features make, MIDI thru, MIDI learn with savable controller maps (`zederia.ccmap`), program change from a program list.
  - Plug-in: MIDI out and thru (it now produces MIDI), MIDI learn, program list, rack editing and presets, clips, offline-quality detection, state version 2 with the instance's own settings.
  - `zederia-cli`: `clip import|export`, `render --midi-out`, `bench --profile` and new scenarios (`idle_rack`, `fx_tail`, `preset_load`; `full8` and `full8_draft` now include the six effects).
  - Optimization pass: lanes that nothing moves keep their values instead of being re-aimed at every tick; a lane resting on a frame reads one frame instead of two; half the reverb's lines stay still; slow modulators use a cheap sine; a rack that is all asleep and a silent output skip their work; idle and switched-off effects skip their parameter checks.
  - Tests: 276 (core, plug-in, command line), all under the audio-thread guard, plus golden fingerprints of six reference patches. pluginval passes at strictness 10 and clap-validator passes (`scripts/test.ps1 -ClapValidator`).
- Milestone 5 (the interface), tagged `m5-done`:
  - Component kit and theme tokens (`plugin/ui/`): knob, fader, number box with unit-aware typing (`2k`, `-6dB`, `1/8.`, `A#3`), switch, choice menu, segmented choice, button, tabs, collapsible card, drag chip, tooltips and the help strip, meters, and one curve editor for LFO shapes, multi-segment envelopes, warp curves, Drive's curves and route curves (points, curve handles, grid and snap, steps, multi-select with proportional moves, a shape library, copy and paste). Displays: wavetable in 3D and 2D, filter response, envelope, output spectrum, signal map. All vector, scaled freely from 70 % to 300 %; themes Graphite, Paper and Contrast as token files, more from `Documents/Zederia/Themes`; a component gallery page (KIT) for review.
  - `zederia-cli ui-snapshot`: any page, scale and theme rendered offscreen to PNG, with `--audit` checks for clipped, overlapping, unlabeled, zero-size or cramped controls, truncated or too small text and theme contrast.
  - Pages: SYNTH (source cards, the Stage display that follows focus, filters, routing, voice, glide, character, tuning, output; the modulator dock or the keyboard), FX (three lanes, drag within and between lanes, the selected slot's controls and display, buses, FX off, rack lock, rack and slot presets), MOD (every route with search, sort, multi-select for bulk source replacement, mute and removal, Via, amount, polarity, curves), TABLE (the workshop: frame strip, wave drawing tools, harmonic, spectral and formula views, process, morph, sort, resynthesis, export, and the import wizard), PLAY (arpeggiator with the step lane and twelve patterns, chord memory, scale lock, strum, clip player with a piano roll and MIDI file import and export), LIBRARY (facets, search with `field:value` filters, sortable results with a second key, audition, bulk tags, ratings, favourites, move and pack export, Blend, pack and folder import with duplicates found by sound, module presets) and the AGENT shell.
  - Modulation as in plan 3.7: drag a source chip onto any control, depth arcs and the live value on every modulated control, a hover list of a control's routes with their amounts, polarity, mute, solo and remove; Alt-drag sets depth, Shift+Alt-click flips polarity; the mod map draws every connection on the page.
  - The editing session: named undo and redo over parameters, routes, shapes, wavetable edits, rack edits and preset loads, with a history list to jump to any step; A/B with copy across; parameter locks; Randomize, Mutate and Blend by scope; copy and paste of every module and shape.
  - Top bar (preset browse and save, undo, history, A/B, quality, CPU, FX off, agent state, output meter), command palette (Ctrl+K: any control, action, page or preset), right-click menus on every control (type a value, reset, MIDI learn, lock, copy, modulation), Simple view, preferences, keyboard access and screen-reader names and values for every control, keyboard shortcuts.
  - Performance: one frame clock capped by the preferences, repainting only the part of a widget that changed; nothing animates while the window is hidden; software rendering unless the preferences allow the graphics card; workshop tables built on a worker thread, incrementally.
  - Core support: tables embedded in patches, typed values, history, variation, module copy and paste, the preset index and query language, incremental table builds.
  - Tests: 300 (core, plug-in, command line), all under the audio-thread guard. pluginval passes at strictness 10 with the editor, and clap-validator passes.

- Interface revision after milestone 5 (2026-11-18, from hands-on feedback), on branch `m5-interface-layers`:
  - **Four fixed layers** on every page: the header (logo plate, preset bar, page tabs), the page, the modulation dock (the eight macros; envelopes, LFOs and Flux with their chips, display and controls) and the keyboard (pitch and mod wheels, octave and velocity for the computer keys, the keys, Keys switch, Panic). The base size is now 1320 x 940 (was 1320 x 820); the macro strip is gone (the macros are in the dock).
  - **Hardware finish**: machined panels with grain, a bevel and a drop shadow, engraved header plates, screws with hex sockets, metal knob caps with knurling, lit power buttons, inset display wells, an ivory-and-lacquer keyboard. Two theme metrics drive it (`texture`, `metal`): Graphite full, Paper lighter, Contrast without grain.
  - **SYNTH page reorganized**: the three oscillators side by side, each with its own table display, its main controls on one row and the rest in four tabs (Unison, Spread, Warp, Pitch); Sub and Texture below; the Stage beside them (filter response, warp curves, Sub or Texture output) with a button that opens the signal map large over the oscillators; filters and voice on the right. Nothing collapses any more.
  - **FX page as a rack**: the chain (lane tabs, the lane's effects in order, drag to reorder or onto another lane's tab, Lane B parallel or in series) and the rack card on the left; on the right every effect of the lane as a strip with its controls in named groups (for example Delay: Delay, Left, Right, Feedback, Movement), its mix, and a display where one helps (EQ and Filter responses, Drive's curves, the Widener's correlation). Controls that do nothing in the current setting hide (rate or division by Sync, the right side while Link is on, the compressor's bands outside multiband, Drive's filter while it is off and its curve controls outside the curve modes).
  - **Computer keys** in two rows: Z S X D C V G B H N J M , L . ; / play from C of the chosen octave and Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P [ = ] from C an octave higher (where the rows overlap two keys play the same note); Left and Right move the octave, Down and Up the velocity, so no key that plays a note changes them; Shift does not change the note, number-pad keys play nothing; held keys repeat without retriggering; letters typed into a text field stay there. On by default; the Keys button and the preferences switch it, and the preferences show or hide the letters on the keys (Key letters). An octave and velocity saved with the earlier one-row layout (A W S E ..., whose Z, X, C and V moved the octave and velocity while playing) are not kept: the keys start again from C3 at velocity 100.
  - **No highlight on a click**: a click never draws an outline. A click on a control gives the keyboard back to the editor (so the computer keys keep playing and Space, Delete or the arrows never act on the last control clicked); parts that edit with keys of their own (curve editors, the step lane, chord notes, piano roll, frame strip, wave, harmonic and spectral editors, effect slots) keep the focus but show it only once an editing key is used. Tab and the command palette still show where the keyboard is. The selected source or filter card no longer gets an outline; text fields show the caret only.
  - **Header**: the Undo and Redo buttons are gone; Ctrl+Z undoes, Ctrl+Shift+Z (or Ctrl+Y) redoes, and the history list and the command palette remain.
  - **Pitch wheel**: the wheels no longer send every move through the keyboard's event queue. The processor reads their latest position each block, so a full queue or a host that stops calling the audio thread while the wheel moves can no longer leave the pitch bent while the wheel shows the centre. The pitch wheel also springs back when the host keeps the mouse release from the window (checked every frame) and when the editor closes; a reopened editor shows the modulation wheel where it was left.
  - **Stuck notes fixed**: the keys count what holds each note (the mouse, any computer key) and end it with the last; a release the window never receives (mouse let go outside the window or kept by the host, a key let go while another window had focus) is caught every frame from the real button and key state; hiding or closing the editor lets go of everything; and the processor itself releases any note the screen started but no longer holds, so a release lost in a full queue still ends the note.
  - **Fits the screen**: the editor shrinks on its first appearance, if it must, so the whole window including the keyboard fits the display.
  - Fixed on the way: the filter display repainted every frame while idle (it hashed its settings including padding bytes); the PLAY page's chord panel was clipped at the new height.
- Interface follow-up (2026-11-19, from hands-on feedback), on branch `ui-library-agent-setup`:
  - **Knob highlight that stayed**: a control drew its held value in the accent and its cap ring while hovered, but repainted neither when a click ended without moving the value nor when the pointer left. Any repaint during the click (the focus hand-back) left the accent drawn until something else repainted it, so a row of clicked knobs stayed lit. Controls now repaint when a gesture starts and ends and when the pointer enters and leaves.
  - **Text size** in the preferences: Small 90 %, Normal 100 %, Large 115 %, Largest 130 %, live, independent of the window size. Where a larger text has no room it gives back as much as it must, never below the normal size, so nothing is cut off that fits at 100 %.
  - **LIBRARY**: the left column starts with PRESETS, the Presets folder as a tree (All presets, then each folder with its count; folders with folders inside open and close with an arrow; choosing a folder shows its presets and those of the folders inside it). Every section (PRESETS, SHOW, TYPE, CHARACTER, GENRE, PACK, AUTHOR) folds away from its heading. The library makes the default folders when it opens: `808`, `Bass`, `Synth`, `Lead`, `Pad`, `Pluck`, `Keys`, `Arp`, `FX` for the free presets that come with Zederia (empty for now) and `User Presets` for your own. New folder (inside the chosen folder, or User Presets) and Show in Explorer in the toolbar; imported packs go to `User Presets/<pack name>`.
  - **A click loads a preset**: one click (or the arrow keys) on a preset loads it; Ctrl and Shift build a selection for bulk edits and load nothing. The Audition and Load buttons are gone (double-click and Enter still load).
  - **Save preset** asks for name, location, type and tags. The location is a folder inside `Documents\Zederia\Presets`, chosen from a list of the existing folders or typed (`User Presets/My Phonk Pack` makes the folder); it starts at User Presets, or at the current preset's folder when that preset is already one of yours. Replacing another preset of the same name asks first. The library shows the new preset at once.
  - **AGENT, Client setup**: one line and a Connect button. Connect opens a panel with Claude Code, Codex and Antigravity, each with where its configuration goes, a preview, **Copy config** (`.mcp.json`, `config.toml` with the `[mcp_servers.zederia]` table, `mcp_config.json`) and **Copy prompt** (a request to paste into that agent: check the server file, add Zederia to its own settings, confirm, and say when to restart). The panel also has the link switch. The preferences' Agent link row shows the link's state.

- Milestone 6 (the agent bridge), tagged `m6-done`:
  - **Protocol revisions supported: 2026-07-28** (the current revision: stateless, `server/discover`, per-request `_meta`, `resultType`, cache hints, the HTTP headers) **and 2025-11-25 and 2025-06-18** (the `initialize` handshake). Both generations are served by one process and on one HTTP endpoint at the same time.
  - **Clients verified:**
    - **Codex CLI 0.170.0** (handshake revision 2025-06-18) ran example sessions 1 and 3 of plan 4.10 end to end. The preset, the loops and the MIDI files exist (Measurements).
    - **Claude Code 2.1.322** (it probes with `server/discover` at 2026-07-28) connects and lists the tools (`claude mcp list`: Connected). Its command-line program is not signed in on this machine, so it ran no session; see Known issues.
    - The Antigravity CLI is not installed.
  - `zederia-mcp`:
    - stdio (the default) and Streamable HTTP on 127.0.0.1 (`--http`), with an Origin check, a Host check and a bearer token.
    - 26 tools with input and output schemas, annotations and structured results; validation errors carry JSON pointers and fixes.
    - Progress and cancellation for renders and pack exports.
    - Resources (the guide, the patch schema, the parameter registry; templates for presets, wavetables and renders) and five prompts.
  - Headless sessions (`session:...`) and the live link (`live:...`): each instance with Agent Link on opens a pipe only the current user can open and writes a user-only discovery file with a random token. Requests are queued to the message thread and applied through the editing session (so they reach the audio thread at block starts like any edit). Renders of a live target run in the server on a copy of its patch.
  - Core: the analysis module (Appendix C: level, loudness to ITU-R BS.1770, time, pitch, spectrum, motion, stereo, tags), `compare_audio`, mel spectrogram PNGs, the music-theory helpers (chord symbols, roman numerals, 14 scales, voice leading, quantizing), clip checks, pack file names, PNG writing, progress, cancellation and a minimum length in the offline renderer, and recipe steps applied to an existing table.
  - Music tools: `theory`, `write_clip` (validation; into a target's clip player, optionally switched on), loops with tail cut, separate or fold plus the `.mid`, and `export_pack` with the naming rule, one-shots and multi-sampled notes.
  - Safety as plan 4.8:
    - permissions on the AGENT page, stored in `%APPDATA%\Zederia\agent.json` and read on every check;
    - the file sandbox (canonical paths, junctions resolved, `..`, network, device and stream paths, reserved names and unknown extensions refused);
    - limits on render length, tails, sessions, pack size, clip size, request size and formula complexity;
    - every mutating call is one undo step labelled with the client's name, and "Revert agent session" undoes everything an agent did;
    - auditions are level-capped on the audio thread.
  - The AGENT page: link switch, connected clients, activity log, revert, permission switches with allowed folders and the audition cap, and copy buttons for the Claude Code (`claude mcp add` and `.mcp.json`), Codex and Antigravity snippets. The top bar's indicator follows the link.
  - `docs/AGENT_GUIDE.md`, compiled into the server as `zederia://guide/agent`, covers parameter semantics, recipes, the design loop, reading the analysis, loudness and naming conventions, music tools, wavetables, permissions and worked examples.
  - Tests: 336 (core, plug-in, server), all under the audio-thread guard. They include scripted sessions per revision and transport (in-process, the real program on pipes and on files, HTTP) and every tool against its schemas. They also cover error paths, sandbox escapes including a junction, token, origin and host checks, cancellation on stdio, in-process and over HTTP, the live link in-process while audio plays, and the live link against a running Standalone.

## Measurements
### Milestone 6
Measured 2026-11-19 on the machine in `docs/ENVIRONMENT.md`, Release build unless stated.

| What | Result | Target |
|---|---|---|
| Tests | 336 of 336 pass in Debug (434 s; timing budgets are checked in optimized builds only). In Release, 334 of 336 pass in the final runs: the two interface timing tests fail under the machine's load at the time (row below; they passed in the first full run with this milestone's plug-in code) | all |
| Protocol revisions | 2026-07-28, 2025-11-25 and 2025-06-18, each in scripted sessions: in-process, the real program on its standard streams (both with input held open, one request at a time, and with whole input files), and over HTTP (2026-07-28 and 2025-11-25) | both generations |
| Tool schemas | all 26 tools: input and output schemas use only keywords the validator understands; every tool called at least once in the conformance test, every structured result valid against its output schema, no output-schema failure logged | all valid |
| Error paths | invalid arguments (unknown fields with suggestions, wrong types, enum typos, missing fields), unknown tools and methods, unsupported version (-32022 with the supported list), missing capabilities (-32602), bad log level, batches, parse errors, wrong ids, header mismatches (-32020), each with a JSON pointer and a fix where the agent can act | reported with fixes |
| Sandbox | `..` (plain and inside a folder), absolute paths outside, UNC, `\\?\` and `\\.\` paths, alternate data streams, CON, NUL and COM1, trailing dots and spaces, wrong extensions, NUL bytes, empty paths and a junction pointing outside are all refused. Writes are refused when the AGENT page switches them off, and a read folder works only while "Read samples" is on | all refused |
| Token, Origin, Host | missing, wrong or Basic credentials get 401 with `WWW-Authenticate`. A foreign Origin, an Origin that only starts with `127.0.0.1` and a non-loopback Host get 403. A loopback Origin passes. Binding to 0.0.0.0 and tokens shorter than 16 characters are refused | refused |
| Live-link token | wrong, missing and old-protocol tokens refused; a stale patch hash is refused as "changed meanwhile" and the server retries | refused |
| Cancellation | a 110 s render of an 8-note chord on three unison-16 oscillators (about 14 s of work), cancelled after its third progress notification: stopped and gave no response. In-process the whole test took 0.15 s including setup; on stdio (`notifications/cancelled`) 0.21 s; over HTTP (closing the event stream) 0.68 s. No file was written | stops promptly |
| Progress | rises strictly in every notification (rounded to 0.001 before the check) | increasing |
| Live link, in-process | agent edits reach a processor while its audio runs on another thread: parameters, routes, a clip into the clip player (switched on), and an audition (level-capped). Each step is labelled "Tests: ..." with origin "Tests", undone by the agent and by the session's undo (the user's Ctrl+Z), and "Revert agent session" restores the state from before the first edit. Permissions switched off on the AGENT page refuse edits and auditions at once. The audio-thread guard reports nothing, and the link state survives the host project | live, undoable |
| Live link, running Standalone (`[standalone]` test) | started with the link on and its own folders, found through its discovery file (pid and host match). Edits read back, undo, render from a copy of its state (pitch A3 found), audition, and revert | live, undoable |
| Live link by hand (the user's installed Standalone, computer use) | Agent Link switched on from the AGENT page. A server edit labelled "Darker filter" (Ladder LP 24 at 650 Hz, resonance 40 %) and a route from LFO 1 appeared on the SYNTH page with the route's arc. The AGENT page listed the client "Verification" (5 requests, 2 edits) and both actions by name, and the top bar read "Agent on". Two Ctrl+Z restored SVF LP 12 at 2.00 kHz. Switching the link off removed the discovery file | appears, undoes |
| pluginval v1.0.4, strictness 10, Release VST3 with the editor | SUCCESS | pass |
| clap-validator 0.4.1, Release CLAP | 44 tests: 32 passed, 0 failed, 11 skipped, 1 warning (the same as milestone 5) | pass |
| Interface timing tests (`[perf]`) | passed in the first full run with the milestone's plug-in code in (334 of 334). In the later runs they read 2.4 to 3.4 % (budget 2 %) and a worst workshop latency of 55 to 68 ms (budget 50). At those times the Claude desktop app drawing this session used a third of the machine's eight cores and the idle benchmark read 0.036 % against its 0.030 % baseline. **Not a regression:** the milestone 5 build (`m5-done`, built again from a separate worktree) was run alternately with this one on the same machine at the same time:
- Interface cost, five rounds with this build first: 2.05 to 2.54 % (mean 2.27) here against 1.90 to 2.67 % (mean 2.27) for milestone 5.
- Workshop latency, four rounds: feedback 5.7 to 30.0 ms here against 6.5 to 30.1 ms, and new audio 19.3 to 63.0 ms here against 16.3 to 85.8 ms.

Both builds miss the budgets under this load; see Known issues | 2 %, 16 ms, 50 ms |

**Real clients.** Each was registered and checked:
- **Claude Code 2.1.322**, registered for this project (`claude mcp add -s local`). Its health check connects (`server/discover` at 2026-07-28, then the tool list). Its command-line program reports "Not logged in", so no session ran from it here.
- **Codex CLI 0.170.0**, given the server per run. It first ran a check over the handshake revision 2025-06-18: `get_status`, which lists 2026-07-28, 2025-11-25 and 2025-06-18; `theory` for the scale; and a render with the analysis tags.

Codex then ran the example sessions of plan 4.10. Prompts and transcripts are in `build/agent-sessions/` (not in the repository).

*Example session 1: "warm, slightly detuned pluck for a house track".*
- Codex read the guide, used `describe_synth`, `list_wavetables` and `list_presets`, opened sessions, and rendered a four-note A minor phrase at 124 bpm. It made nine renders in all with seven `set_params` rounds, then saved `user:Agent/Amber Step Pluck`: five lightly detuned saws, a ladder filter, a short plate, a "Tone" macro, tags `warm`, `plucky`, `detuned`, `wide`, `analog`, `house`.
- The analysis moved from −21.9 to −13.5 LUFS, a peak of −7.7 to −2.8 dBFS, a centroid of 360 to 764 Hz and a width of 0.110 to 0.267 (`wide`), with 0 clipped samples. The tags changed from `dark` and `sustained` to `plucky` and `wide`.
- Files: `Documents/Zederia/Presets/Agent/Amber Step Pluck.zpreset` and `Documents/Zederia/Renders/ZED_pluck_AmberStep_Am_124bpm_{Before,Wavetable,Warmth,Level,After,ToneMax}.wav` plus three A3 one-shots, each with its analysis sidecar.
- `apply_patch` and `set_modulation` were refused by Codex's approval rule for tools marked destructive. Codex worked round it by passing the whole patch to `open_session`. Those annotations were corrected afterwards (Decisions).

*Example session 3: "eight bars of chords in F minor at 124 BPM and a melody over it".*
- Codex used `theory` (a voice-led progression with a root bass: Fm7, D♭maj7, A♭maj7, E♭7, two bars each) and `write_clip` twice: "chords", and a "melody" from F4 to C6 in two-bar phrases. Neither clip had warnings.
- It found the session 1 pluck with `list_presets` and designed and saved a second preset, `user:Agent/Velvet Tine` (soft keys), with four analysis rounds. It rendered each clip with both presets (tail `fold`).
- Four loops, each exactly 8 bars (15.484 s) with 0 clipped samples: `ZED_chords_AmberStepPluck_Fmin_124bpm` (−17.7 LUFS, peak −2.5 dBFS), `ZED_melody_AmberStepPluck_Fmin_124bpm` (−12.4 LUFS, −1.2 dBFS), `ZED_chords_VelvetTine_Fmin_124bpm` (−12.6 LUFS, −1.6 dBFS) and `ZED_melody_VelvetTine_Fmin_124bpm` (−12.4 LUFS, −5.8 dBFS).
- Files: each loop's `.wav` and `.mid` in `Documents/Zederia/Renders/`, the two clips in `Documents/Zederia/Clips/Agent/`, and `Velvet Tine.zpreset` in `Documents/Zederia/Presets/Agent/`.
- Codex's approval rule refused only `close_session`, which is marked destructive.

### Interface follow-up (2026-11-19: knob highlight, text size, library folders and click to load, save locations, agent Connect panel)
Release build. Nothing under `core/` or `mcp/` changed.

| What | Result | Target |
|---|---|---|
| Tests | 339 of 339 pass in Release, including three new editor tests (folders and saving, text sizes, the Connect panel) and the library test's click-to-load checks; after the last change (save location, the preferences' Agent link row) the 22 editor tests were run again and pass | all |
| Library (`[editor][library]`) | the default folders are made and ordered (free folders first, User Presets last); typed locations resolve inside Presets (relative, `\Zederia\Presets\...`, a full path inside; `..` dropped; a folder outside refused); Kick saved to `User Presets/My Phonk Pack` with type and tags and shown at once; a folder shows its presets and its sub-folders'; closing a folder hides its folders, choosing a folder inside opens the way; folded sections, open folders and the chosen folder survive the page state; a click loads, Ctrl-click and selections made in code do not | pass |
| Text sizes (`[editor][text]`) | all seven pages at 115 % and 130 % text: 0 snapshot findings (no clipped, truncated, overlapping or unlabeled parts); the setting survives the settings file | 0 findings |
| Snapshots | every page at 100, 150 and 200 % and in the three themes, the overlays, the Connect panel (`overlay_connect_100.png`) and the library with folded sections and a deep folder (`library_100_folders.png`): 0 findings | none |
| pluginval v1.0.4, strictness 10, Release VST3 with the editor | SUCCESS | pass |
| Standalone by hand (computer use) | middle-click Pan, click Fine, click Width, middle-click Blend on OSC 2, then move away: no value or cap ring stays lit; LIBRARY shows the new folders in `Documents\Zederia\Presets` (Agent 7, User Presets 0); a click on Velvet Tine loads it; TYPE folds; the Agent folder shows its 7; Save shows Name, Location (list of the folders), Type and Tags (cancelled, nothing written); Connect opens the panel with the three clients (copy buttons not pressed); Text size steps live through 115 and 130 % and back to 100 % | - |

### Interface follow-up (2026-11-19: no click highlight, header without Undo and Redo, pitch wheel, two-row computer keys)
Release build. Nothing under `core/` changed; the golden fingerprints of the reference patches are unchanged.

| What | Result | Target |
|---|---|---|
| Tests | 302 of 302 pass in Release, including the new wheel test | all |
| Wheel (`[editor][wheel]`) | up and let go (engine bend back to 0, the played pitch back to where it was); 600 moves with no audio block between them; the keyboard's queue full; a mouse release that never arrives; closing the editor while held; the modulation wheel kept across reopening. With the wheel put back on the old queued path the same test fails four times (the pitch stays bent while the wheel shows the centre) | pass |
| Keyboard (`[editor][keys]`) | every key of both rows plays its note, overlapping keys hold one note until both are let go, Shift keeps the note, number-pad keys and Ctrl are not notes, playing every key leaves the octave and velocity as they were, the arrows move them | pass |
| pluginval v1.0.4, strictness 10, Release VST3 with the editor | SUCCESS | pass |
| Standalone by hand (computer use) | clicks on Filter 2 Drive, the OSC 3 header, its Table switch and the SYNTH tab draw no outline; after clicking a knob the Right arrow moves the octave and leaves the knob alone; Z X C V play notes and leave Oct 3 / Vel 100; the pitch wheel returns to the centre on release; the header shows no Undo or Redo | - |

### Interface revision (2026-11-18)
Release build, the machine in `docs/ENVIRONMENT.md` at its normal speed (`bench idle` 0.032 % against the 0.030 % baseline).

| What | Result | Target |
|---|---|---|
| Tests | 301 of 301 pass in Release, including the new keyboard test | all |
| Snapshot checks: 7 pages at 100, 150 and 200 % in Graphite, at 100 % in Paper and Contrast, Simple view, four overlays | 0 findings | none |
| pluginval v1.0.4, strictness 10, Release VST3 with the editor | SUCCESS | pass |
| Keyboard (`[editor][keys]`) | click, glissando, drag off the keys, a mouse release that never arrives, a computer key held with repeats, two keys let go while the window had no key events, octave and velocity keys, Ctrl+letter not a note, Keys off, a release lost in a full queue (400 events), letting go of everything, closing the editor with a key held: no note left sounding in any case | none hang |
| Independent review (Codex CLI, read-only, of the uncommitted diff) | nine findings; fixed: a synthesized release could later swallow a host note of the same key (the late real release is now dropped), keys were not checked while the window was hidden, a layout pass rebuilt the effect strips mid-drag, a chip cache key collided with its colour, the filter display ignored quality and sample-rate changes, Drive showed curve B and Morph in single-curve mode, file dialogs could call back into a closed editor, and the signal map was too small in the Stage (now a large panel over the oscillators). Not fixed: a same-key retrigger lost when the queue is full while audio is stopped | - |
| Standalone by hand (computer use) | a click lights and plays the key and ends on release; dragging across the keys moves the note; dragging off the keys ends it; Z and X change the octave in the real window; the window opens fitted to a 1456 x 819 screen with the keyboard visible | - |
| Interface while animating (`[editor][perf]`, SYNTH page, two notes, two LFOs, 30 fps, 100 %) | 1.5 to 2.0 % of one core in nine runs on a slow stretch of this machine (frame clock 0.27 to 0.38 %), 18,200 px repainted per frame; one run started while the build was finishing read 2.06 %. Before the caching below: 8.7 %; the old flat interface measured 1.4 to 1.8 % | 2 % |

### Milestone 5
Measured 2026-11-18 on the machine in `docs/ENVIRONMENT.md`, Release build unless stated.

| What | Result | Target |
|---|---|---|
| Tests | 300 of 300 pass in Release (28 s) and in Debug (182 s) | all |
| pluginval v1.0.4, strictness 10, Release VST3 with the editor (its Editor, Open editor whilst processing and Editor Automation tests included) | SUCCESS | pass |
| clap-validator 0.4.1, Release CLAP | 44 tests: 32 passed, 0 failed, 11 skipped, 1 warning (the same as milestone 4) | pass |
| Snapshot checks: 7 pages at 100, 150 and 200 % in Graphite, at 100 % in Paper and Contrast, Simple view, and four overlays (palette, preferences, history, mod map) | 0 findings (clipped, overlapping, unlabeled, zero-size or cramped controls, truncated or small text) | none |
| Theme contrast, three themes | every text and meaning colour passes | pass |
| Reachability | every one of the 864 registry parameters has a control on some page, except the 24 sixteenth values of the effect slots, which no effect uses | all |
| Undo and redo | a knob gesture, a route dropped from a chip and its depth set by Alt-drag, an LFO shape, a wavetable stroke, three rack edits (insert, insert, move), a preset load, a value on the B side, the A/B switch and Randomize: undone step by step and redone, the patch equals the recorded state at every step; the steps carry their names | exact |
| State | the page, the scale, the selected effect slot, the patch, A/B's other side and the locks survive closing and reopening the editor, and the host state restored into a new instance; a locked parameter keeps its value when a preset loads | survives |
| Interface while animating: SYNTH page, two notes, LFOs on oscillator position and filter cutoff, 3D table view, 30 frames per second, 100 %; offscreen paint of the repainted areas into an image the size of the editor (`[editor][perf]`) | 1.44 to 1.76 % of one core in six runs (frame clock 0.16 to 0.20 %); about 47,000 pixels repainted per frame. Earlier in this milestone, before the cached 3D view and partial repaints: 5.7 % | 2 % extra |
| Interface idle | nothing repaints when nothing moves; hidden, the clock only checks twice a second | idle |
| Standalone, real window at 150 % display scaling (1983 x 1272 pixels), software rendering, idle | the editor draws completely and sharply; message thread 0.2 to 0.8 % of one core, whole process with the audio device 0.9 to 2.2 % (Windows samples thread time in 15.6 ms ticks, so these are rough) | - |
| Workshop feedback: one drawing stroke to the wave view repainted | at most 5.9 ms (64, 256 and 16 key frames) | 16 ms |
| Workshop audio: stroke to the new table playing (build plus one 512-sample block) | 14.1 ms (64 key frames, crossfade), 19.3 to 20.0 ms (256 key frames, none), 26.8 ms (16 key frames, spectral: 256 positions); earlier in this milestone 51 ms (256, none) and 45 ms (spectral) | 50 ms |

Benchmarks (Release, one core, 48 kHz, 512-sample blocks; `scripts/bench.ps1`, 5 runs of 20 s, against the milestone 4 baseline, which is unchanged): every scenario passes. idle 0.030 %, idle_rack 0.048 %, one_voice 0.214 %, one_voice_rack_off 0.231 %, chord8 2.48 %, chord8_mod 2.85 %, full8 12.9 %, full8_draft 12.5 %, fx_tail 0.96 % (late/early 0.66), preset_load 17.3 ms (was 28.3 ms; the 64-frame table build 14.3 ms, was 26.3 ms, from this milestone's faster table builds). The engine itself did not change in this milestone; the other differences from the baseline are the machine's variation.

Snapshots for review (written by the editor tests): `build/release/snapshots/` holds `<page>_<scale>.png` for the seven pages (`synth`, `fx`, `mod`, `table`, `play`, `library`, `agent`) at 100, 150 and 200 %, `<page>_100_paper.png` and `<page>_100_contrast.png`, `synth_100_simple.png`, and `overlay_palette_100.png`, `overlay_prefs_100.png`, `overlay_history_100.png`, `overlay_modmap_100.png` (40 images). Regenerate them with `scripts/test.ps1 -Filter "every page renders"` or `zederia-cli ui-snapshot --page all --theme all --audit --out <folder>`.

### Milestone 4
Measured 2026-11-16 and 2026-11-17 on the machine in `docs/ENVIRONMENT.md`, Release build unless stated.

| What | Result | Target |
|---|---|---|
| Tests | 276 of 276 pass in Release (32 s) and in Debug (197 s) | all |
| pluginval v1.0.4, strictness 10, Release VST3 | SUCCESS | pass |
| clap-validator 0.4.1, Release CLAP (`scripts/test.ps1 -ClapValidator`) | 44 tests: 32 passed, 0 failed, 11 skipped (a Unix-only check, no preset-discovery factory, no 64-bit audio, no audio inputs, fixed port layouts), 1 warning: 1 MB of random bytes "load" as state without an error (the framework's state call has no failure return; the plug-in ignores such data and keeps its state) | pass |
| Reverb decay at 0.7, 2 and 5 s, plate, hall and room (damping 0, cuts wide open; T60 from the -5 to -35 dB slope of the energy decay) | 0.650 to 0.663 s, 1.856 to 1.877 s, 4.664 to 4.695 s: 5 to 7 % short of the setting | within 20 % |
| Reverb click test: the tail's spectrum (eight windows from 100 ms) against its own sixth-octave median | strongest line 8.6 dB (plate), 5.3 dB (hall), 8.1 dB (room) above it, as noise does | below 10 dB: no metallic ringing |
| Reverb mono compatibility (noise in): correlation of the two sides; mono sum against the sides' mean | -0.027, -0.014, -0.025; -3.13, -3.07, -3.12 dB (two uncorrelated sides: -3.01 dB) | mono-compatible |
| Reverb freeze: level 1.5 s and 9.5 s after freezing | -20.14 and -20.43 dB (before the fix in this milestone: -1 dB every 3 s) | holds |
| Limiter effect at -6 dB on noise peaking far above full scale; ceiling moved during a loud passage | highest output -6.00 dBFS; never above either ceiling | at or below the ceiling |
| Output protection on a four-note chord at +12 dB master | soft clip at most 1.0, limiter at most -0.3 dBFS (a peak above 1.5 with protection off) | at or below full scale |
| Drive hard clip on a 5 kHz sine, worst alias | -21.7 dBc at 1x, -53.7 dBc at 4x | falls with quality |
| Level and tail of the other effects (`test_fx.cpp`) | Drive: unit gain at no drive within 0.3 dB, every mode bounded and silent 250 ms after its input; Compressor: the static curve within 1.5 dB, three-band crossovers flat within 0.3 dB, upward compression lifts quiet material; EQ and Filter: measured gains within 0.1 and 0.5 dB of their analytic responses; Chorus: level within 3 dB, sides correlated below 0.9, silent 50 ms after its input, stable at full feedback; Flanger and Phaser: notches deeper than 20 dB where the delay and the all-pass chain put them, stable at +/-0.97 feedback; Delay: first echo at 375 ms to the sample, second -9.1 dB lower within 0.5 dB, ping-pong and pre-tap where they belong, ducking more than 20 dB; Swarm: level within 3 dB, energy moved off the input's pitch; Widener: mono sum unchanged within 1e-5 | as expected |
| Clicks: six rack edits while a sine plays (insert at the front, insert in lane B, reorder, move to Master, remove, insert a drive); largest sample step in the 50 ms after the edit against the steady signal before and after | 0.58 to 1.05 times | at most 1.5 |
| Clicks: power off and on, lane B to series, FX off and on | 0.32 to 0.91 times | at most 1.5 |
| Clicks: quality changes while a note plays through a warped oscillator, Drive, a ladder Filter effect and the soft clipper (to High, Ultra, Draft, Standard, and the offline switch) | 0.999 to 1.000 times | at most 1.5 |
| Idle | a delay, a reverb and an EQ all idle within 4 s of the note's end, output exactly zero, wake when a note comes (the delay first, what follows it when its echo arrives); a frozen reverb never idles; a switched-off effect never runs | idle after tails |
| Latency | reported 0; in all four quality modes, with and without Drive, EQ and Compressor, nothing before the note and sound within 40 samples of it | correct |
| Arpeggiator | every mode's order exact over eight steps (Random the same for the same seed, Walk always to a neighbour); note-ons on the 1/16 grid to the sample; swing, triplet rate, gate, ties, rests, transposition, velocity, ratchets, latch, restart and free running exact | exact |
| Chord memory, scale lock, strum | exact notes; strum steps of 10 ms to the sample; a key let go before its strummed turn never sounds | exact |
| Clip player against its source MIDI file | 24 random notes through a file and back: starts within half a tick, keys exact, velocities within half a step; played on the host's beats, every note at its sample; key trigger transposes, stops on release, latches, plays once without loop | exact |
| MIDI out | only the notes the play features made; nothing with every feature off or with MIDI out off; in the host's buffer from the plug-in; thru passes incoming events unchanged | as specified |
| Host state | every host parameter set at random, saved, restored into a new instance: all equal, and saving again gives the same bytes | identical |
| Determinism | an arpeggio through Chorus, Delay and Reverb renders the same hash twice and at block sizes 512 and 64; `zederia-cli render` of `tests/data/patches/arp_fx.zpreset` twice gives identical files | identical |
| Golden renders, six reference patches | within 0.05 dB (frame levels) and 0.1 dB (band levels) of the stored fingerprints in Release and in Debug | match |
| Audio-thread guard | no allocation, free or lock across the whole corpus, including rack edits from another thread every 2 ms while two notes play, MIDI learn and MIDI out | none |

Benchmarks (Release, one core, 48 kHz, 512-sample blocks; baseline in `tests/bench/baseline.json`, the slower of two full runs; this is the last full run, 5 runs of 20 s):

| Scenario | Realtime factor | Budget (plan 5.8) |
|---|---|---|
| idle | 0.033 % | 0.2 % |
| idle_rack (all 24 slots filled, after their tails) | 0.050 % | 0.2 % |
| one_voice (one wavetable oscillator) | 0.210 % | 0.5 % |
| one_voice_rack_off (the same with all 24 slots filled and switched off) | 0.234 % (another run: 0.2488 % against 0.2486 % for one_voice) | 0.5 % (switched-off modules cost nothing) |
| chord8 (8 voices, one oscillator at unison 7, one filter) | 2.53 % | 4 % |
| chord8_mod | 2.99 % | 4 % (not a plan row) |
| full8 (8 voices x 3 oscillators at unison 16, two filters, six effects) | 14.2 % | 35 % |
| full8_draft (the same in Draft) | 13.7 % | 20 % |
| fx_tail (a 30-second reverb tail: late against early in it) | 1.03 %; late/early 0.88 | not slower late (no denormal slowdown) |
| preset_load (read and check a patch with six effects, apply it, build a 64-frame table, first block) | 28.2 ms, of which 26.3 ms is the wavetable build | 100 ms |

All budgets are met; the interface rows of plan 5.8 belong to milestone 5. The optimization pass (profile with `zederia-cli bench --profile`): before it, with the six effects already in, `full8` measured 17.9 % (oscillators 11.6 %, per-voice control ticks 2.7 %, reverb 1.5 %, filters 1.2 %, the other five effects 1.1 %), `chord8` 3.26 %, `chord8_mod` 3.72 % and `idle_rack` 0.196 %. Skipping the per-lane re-aim when nothing moves took most of the control-tick cost; reading one frame where a lane rests on a frame cut the oscillators by about a quarter in these scenarios (positions at their default, 0); the still reverb lines and the cheap sine took about a third off the reverb; the sleeping rack, the silent output and the skipped checks of idle and switched-off effects took the idle rack to a quarter. `preset_load` first read 84 ms in a two-run trial and 28 to 33 ms in every run since; nothing in this milestone touched the table builder, so that first reading is not a before-and-after comparison. Against milestone 3, `one_voice` costs a little more (0.195 % then): every block now passes the output stage (master after the rack, the DC filter on by default).

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

Milestone 4:
- **Rack edits are plans.** `Engine::setRack` builds the whole layout off the audio thread with every effect instance named; the audio thread swaps it in. Within a lane, the continuing effects that keep their order (the longest common order of old and new) stay untouched; every other one fades out (10 ms), moves while transparent, and fades back in. New effects fade in, removed ones fade out. A slot's input is scaled by the same fade as its wet signal, so nothing inside an effect sees a step either. While a plan waits for its fades, effects keep their values (the store may already hold the new layout's values at the old positions); a value changed by hand during those 10 ms can be overwritten by the plan's.
- **Slot values are positional** (`fx.a3.p2`) and the engine keeps per value the definition that governs it, so smoothing, routes and `liveValue` work in the loaded effect's units. Routes onto a switch or a choice of an effect are ignored. `Patch` rack edits move values, drawn curves and routes with the effect; routes onto a removed effect are removed. A locked rack keeps its effects and values; the incoming patch's routes onto effects are dropped.
- **Idle**: an effect idles once input and output stayed below -140 dB for longer than it can hold sound (`holdSamples`: a delay's time, a reverb's pre-delay, a filter's ringing). The decision is taken at control ticks and an idle effect wakes at the exact sample where its input passes -140 dB, so idling never depends on the host's block size. A frozen reverb never idles; an effect that is switched off does not run and starts clean when switched back on. Idle state is not cleared (what is left is below -140 dB).
- **What "wet" means**: Chorus, Delay, Reverb and Swarm give only the processed signal; Flanger and Phaser give the dry signal plus the delayed or all-passed one, halved, so that 100 % mix is the full comb or notch effect. The slot's mix crossfades dry and wet.
- **Effect details the plan leaves open**: Drive's amount is 0 to +36 dB of gain (Bit crush: 16 down to 2 bits; Downsample: hold up to 64 samples; both at the base rate on purpose); a 10 Hz DC blocker follows every mode; the filter morphs low-pass, band-pass, high-pass. Compressor ratio 20 is a limiter; in three bands each band compresses by up to 10:1 above its threshold and lifts quiet material by up to 18 dB below it (never below -60 dB). EQ Q from 0.3 to 9.6. Delay offset: -1 is the triplet length, +1 the dotted one; Pre-tap puts one early tap at the left time ahead of the echoes at the right time; feedback band +/-0.3 to +/-5.3 octaves; times glide over about 60 ms. Reverb: line lengths 6 to 31 ms (plate), 24 to 104 ms (hall), 8 to 37 ms (room) at size 50 %, half to twice that over the size range; damping shortens the high-frequency decay to a tenth at most; long decays trim the input gently; a change of mode fades the tail out and lets the new space build. Swarm: copies spread up to +/-25 cents, each wandering a third of that. Widener taps at 3 to 17 ms times Size.
- **Output stage**: master volume after the rack; the DC filter at 5 Hz is on by default, so the engine's reported tail grows by 0.3 s for it to settle, and its tail ends in exact silence below -100 dB. The older tests that measure voices' exact waveforms or exact silence now switch it off in their measurement patches. Output protection: a soft clipper at a fixed 4x in every quality mode (it never has to switch rate), or a look-ahead-free limiter with its ceiling at -0.3 dBFS and an 80 ms release.
- **Quality changes while notes play** duck the voices: 5 ms down, the switch, 1.3 ms of silence while the new oversampling filters settle, 5 ms up. Drive and the Filter effect crossfade between two oversampling paths instead. No effect looks ahead, so the latency stays 0.
- **Offline quality** engages when the host says it renders offline, or when a quarter second of audio arrives in less than half that time; it disengages above 80 % of real time.
- **Play pipeline**: clip trigger, then scale lock, chord memory (notes snapped too), strum, arpeggiator. Strum applies to every note-on that arrives at the same sample (chord memory chords and hand-played chords); a note released before its strummed turn never sounds. Arpeggiator steps follow the song's grid while the host plays, starting a phrase with a step at once and continuing on the grid from the next line at least a quarter step later; with the transport stopped they count from the first key. Swing delays every second step by up to half a step. Step velocity scales the played velocity. A tie step holds the note of the step before it through (only the last ratchet). Clips play at the host's tempo; a key transposes by its distance from the clip's root (the key signature's tonic near middle C on import, otherwise middle C).
- **MIDI out** carries the notes the play features made, on channel 1, only while one of them is on; MIDI thru passes everything that came in. The plug-in writes at most the 2048 bytes the VST3 and CLAP wrappers reserve per block.
- **MIDI learn** binds any channel; a binding for one channel wins over an any-channel one. Moving a bound control sets the parameter on the audio thread; the host is told on the message thread. The controller map, the program list (file paths) and the rack and tuning locks are saved in the host project (state version 2) and restored from it, whatever the instance had locked.
- **Golden renders** store fingerprints, not audio: the level of every 2048-sample frame per channel and a twelve-band spectrum of every 8192 samples, compared within 0.05 dB and 0.1 dB, so that rounding differences between builds pass and any change to a sound fails. `ZED_UPDATE_GOLDEN=1` rewrites them; they were rewritten once in this milestone, after the reverb's lines changed.
- **Benchmarks**: `full8` and `full8_draft` now carry the plan's six effects (drive, chorus, delay and reverb in lane A; EQ and compressor on the master lane). New scenarios: `idle_rack` (all 24 slots filled, after their tails; budget 0.2 %), `fx_tail` (the cost late in a 30-second reverb tail against early in it; at most 1.5 times), `preset_load` (milliseconds to read, check and apply a patch with six effects and build a 64-frame table; budget 100 ms). The baseline holds the slower of two full runs for each scenario, because this machine's speed varies by about 10 % between runs.
- **Optimizations kept bit-exact**: the per-lane re-aim is skipped only when every input to it is unchanged and the ramps have arrived (the steps are zeroed); one frame is read only when every lane of a vector rests exactly on a frame for the whole run. The SIMD kernels stay bit-identical to the scalar one. The reverb's still lines sit on whole samples so their reads are exact.
- **Reverb freeze is lossless**: freezing fades the lines' movement out and puts every line on whole samples, so the frozen sound circulates without loss (0.3 dB over 8 s) and without the slow darkening fractional reads cause. Unfrozen, half the lines move (cubic reads, depth raised by the square root of two to keep the overall movement) and half stay still on whole samples.
- **Host parameters the patch does not use** (the values of an empty slot, the amount of a route that does not exist, an effect's slot values beyond its own parameters) stay at their defaults in the patch, but a host expects back what it set: the plug-in saves such values beside the patch in the host project (`unused_values` in state version 2) and restores them. clap-validator's state tests check exactly this.
- **Switched-off and sleeping effects cost nothing**: an effect that is switched off or idle skips even its per-tick parameter checks until it wakes, and a rack whose effects are all asleep or off is skipped as a whole (one test per block). `one_voice_rack_off` (24 switched-off effects) measures the same as `one_voice` within the machine's noise.
- **clap-validator** 0.4.1 is installed beside pluginval and run by `scripts/test.ps1 -ClapValidator`; its one warning (random bytes load as state without an error) stays, because the framework's state call cannot report failure. The plug-in ignores such data and keeps its state.

Milestone 5:
- **The session belongs to the processor**, not the editor: closing the window loses nothing, and the agent server of milestone 6 can drive the same history. Every interface change goes through it, either as a parameter gesture (one step from press to release, nudges of the same control within 1.2 s merged) or as an edit of a copy of the patch. Changes from the host are absorbed into the history before the next step, so undo never jumps over them.
- **History holds whole patches**, not differences: simple and exact, and `restore` applies only what differs, so a step costs only what changed. It is capped at 256 MB (the oldest steps go first) and lives for the session; the host project keeps the current patch, A/B's other side, the locks and the interface's state, not the history.
- **Locks** belong to the instance (saved in the host state) and act on preset loads, Randomize, Mutate and Blend; a module can be locked as a whole from any of its controls.
- **Software rendering by default.** The editor switches its window to the framework's software renderer unless the preferences allow the graphics card; nothing requires a graphics card, and snapshots and the window draw the same way.
- **One frame clock with partial repaints.** Widgets report what changed and which part (`dirtyArea`), so animation repaints small areas. Things that move every frame are drawn cheaply: the lit line of the 3D table view has no glow (a wide translucent stroke across the surface cost more than the rest of the frame), the view's caption shows the patch's position (the line shows the live one), and the small wave thumbnails in the source cards redraw at half the frame rate. The CPU readout changes at most twice a second and only when its figure changes; it is the one thing the idle-frame test lets repaint, since it shows measured time. Narrow slices along the moving line were tried and measured slower: every drawing call then works through each slice.
- **Interface measurements** paint offscreen into an image the size of the editor, the work a window does minus the system's composition. Plan 5.8's budgets are checked in optimized builds only (a Debug build prints the numbers), and the two measuring tests run alone under `ctest` (`RUN_SERIAL`), since tests beside them would be counted as interface time.
- **Workshop builds** run on a worker thread (`TableBuilder`) that drops a build overtaken by a newer stroke; the table build is incremental (unchanged frames keep their levels; spectral tables regenerate only the positions between changed key frames) and synthesizes two positions per inverse transform.
- **Edited tables travel inside the patch** (`table.data`: float32 frames as byte planes, deflated, base64), so a preset made in the workshop needs no separate file. Texture samples are still referenced only.
- **Themes are token files** compiled into the binary from `plugin/themes/*.json`, plus any found in `Documents/Zederia/Themes`. Paper's faint text, focus and source colours were darkened to pass the contrast check.
- **`zederia-cli ui-snapshot`** links the plug-in's shared code, so the command-line tool depends on the framework when the plug-in is built (it is not available otherwise). Its checks flag is `--audit` (`--check` already takes a file for `params`). Snapshots use default preferences unless `--settings` names a file.
- **Reachability** counts a parameter as reachable when a control for it exists on some page (or in the preferences); the sixteenth value of each effect slot is reserved, since no effect has sixteen parameters, and is not shown.
- **The keyboard replaces the modulator dock** on the SYNTH page when "Keys" is chosen, instead of taking room of its own.
- **Factory favourites** are kept in the preferences, since factory presets are read-only; user presets keep theirs in the file.
- **The AGENT page is a shell** that shows where milestone 6's controls go; it connects to nothing.
- **A destroyed control no longer clears the hover state**: the editor holds the hovered control by a safe pointer, and the hover list follows the routes every frame while it is open, so mute, solo and remove made inside it show at once and it closes with its last route.

Interface revision (2026-11-18):
- **The base size grew to 1320 x 940** so the dock and the keyboard can stay under every page without squeezing the pages; the editor shrinks on first appearance to fit the display, so a 1080p screen at 125 % still shows the whole window.
- **The finish is cached, not cheap**: grain, gradients and bevels cost several times a flat fill, so every static surface is drawn once into an image at the display's density (`CachedDrawing`: cards with their plates, titles and outlines; the header with the logo and the preset window; a knob's cap, and a modulated knob's whole face; modulation chips; the filter display's grid and still curve) and cards are opaque, so nothing behind them repaints. While the snapshot checks record, caches are bypassed so every text is still checked.
- **Moving markers repaint only where they move, and secondary ones less often**: a modulated knob repaints its live dot's old and new places at the full rate; the output meter and the modulator chips' live bars update at half the frame rate, and the filter display's live curve at a third when only modulation moves it (a patch change redraws at once), as the small wave thumbnails already did.
- **Computer keys are handled by the editor**, after its shortcuts and after any focused control or text field, so letters typed into a field never play; keys with Ctrl or Alt are never notes. The keyboard does not take focus when clicked, so typing keeps playing after a click on it.
- **Two guards against hanging notes**: the keyboard checks the real mouse button and key state every frame (a release a host keeps from the window is caught within a frame), and the processor compares the notes the screen holds (an atomic mask set before a note-on is queued and cleared before its release is queued) with the notes it started from the screen, releasing any the screen let go.
- **Effect strips list their controls by name** (`groupsFor` in `FxPage.cpp`); any parameter a table does not name joins a last "More" group, so a new effect parameter is never unreachable.

Interface follow-up (2026-11-19):
- **The free presets' folders live in the user's Presets folder** (`Documents\Zederia\Presets\808`, `Bass` ...), as asked, beside `User Presets`. The library's tree merges folders by name, so factory presets installed in the factory folder under the same folder names appear in the same folders; where milestone 7 installs them is its decision (below, Next).
- **The text size scales the font, not the layout**: boxes keep their sizes and larger text shrinks back toward the normal size where it would not fit. Scaling the layout would change every page's arrangement; the window size already scales everything. The wordmark keeps its size.
- **The snapshot checks judge the size actually drawn**, so a text that gave back its enlargement is not reported, and one too small at 90 % is.
- **A click loads only when it leaves one preset selected** and neither Ctrl nor Shift is held, so bulk selection still works; selections the page makes itself (rescans, Find duplicates) load nothing.
- **Save starts at User Presets** unless the open preset is already in User Presets: saving a free or an agent's preset makes your copy instead of writing over theirs.
- **Editor tests use a content folder of their own** (an empty temporary folder unless the test brings presets), so the library's default folders are never made in the person's Documents by a test.

Milestone 6:
- **The specification was read first** (modelcontextprotocol.io, revision 2026-07-28 and its changelog, the TypeScript schema; revision 2025-11-25 for the handshake). The plan's summary in 4.3 matches it. Where the specification is more precise or differs, it was followed:
  - Error codes come from the reserved range: -32020 HeaderMismatch, -32021 MissingRequiredClientCapability, -32022 UnsupportedProtocolVersion. "Resource not found" is -32602, not the older -32002.
  - Cache hints (`ttlMs`, `cacheScope`) are required on `server/discover` and `resources/read` as well as on the list results.
  - On HTTP in 2026-07-28, closing a request's event stream is the cancellation; `notifications/cancelled` is for stdio, and the handshake revisions use it on HTTP too. `ping` and `logging/setLevel` no longer exist in 2026-07-28.
  - Change notifications come only through `subscriptions/listen`. This server's lists never change while it runs, so it acknowledges no notification types and keeps the stream open until the client closes it or the server ends.
  - Structured results are also given as JSON text ("for backwards compatibility"), after the plan's short summary line.
  - Logging, Roots and Sampling are deprecated in 2026-07-28 and are not used. Multi round-trip input (`input_required`) is not needed by any tool.
  - `UnsupportedProtocolVersionError` lists all three supported revisions, modern first, as the specification's example does.
  - A 2026-07-28 notification POSTed over HTTP (the revision defines none) is accepted with 202 and ignored.
- **Which revisions the installed clients speak was observed, not assumed.** A logging stdio server showed Claude Code 2.1.322 probing with `server/discover` at 2026-07-28 and falling back to `initialize` at 2025-11-25, and Codex 0.170.0 sending `initialize` at 2025-06-18 only.
- **One process serves both generations.** stdio has one handshake state per process; HTTP has one per `Mcp-Session-Id` and none for 2026-07-28 requests. A request is modern when its `_meta` carries the protocol version. Handshake results carry no `resultType` or cache hints (fields their revisions do not define).
- **The protocol code is a library inside `mcp/`** (`zederia_mcp_lib`), so the conformance tests drive it in-process. Nothing of the protocol is in the plug-in or the core.
- **Live edits are whole-patch transactions**:
  - The server reads the instance's patch with a hash, applies the tool's change to a copy, and sends the result with that hash.
  - The instance applies it through `Session::applyAgentState` as one step labelled "<client>: <label>" with the client as origin, and refuses it if its patch changed in between; the server then retries up to three times.
  - The plug-in needs only five requests (`status`, `get_patch`, `put_patch`, `history`, `audition`), and headless and live targets share every tool's code.
  - Edits reach the audio thread the way the interface's do: parameter values through the store at the next block, structure through the hand-overs.
- **The link thread never edits**: it checks the token (constant-time), queues the request and waits up to 8 s. The message thread runs it from an async call, with the processor's 30 Hz timer as a fallback. Renders of live targets run in the server on the copy it read: never on the instance's audio or message thread.
- **Pipe and file security**:
  - The pipe's security descriptor admits only the current user's SID, and remote clients are refused.
  - The first instance is created with the "first instance" flag, and one instance always listens, so the name is never free for another process to take.
  - The client connects at the identification level only.
  - Discovery files and the permissions file are written with the same user-only descriptor, via a temporary file and a replace. Files of processes that have ended are removed when listed.
- **Permissions live in one user-wide file** (`%APPDATA%\Zederia\agent.json`). The AGENT page writes it and the server and every instance read it on each check, so a switch applies at once and to every agent. Command-line `--allow-read` and `--allow-write` folders count as the user's explicit choice and are allowed even while the page's "write in allowed folders" switch is off. Read and edit apply to live targets: a headless session is the agent's own.
- **Annotations**:
  - `readOnlyHint` for the tools that change nothing.
  - `destructiveHint` only where something cannot be got back: `close_session` (the session's history), `tag_presets` (rewrites preset files) and `edit_wavetable` (can rewrite a user table in place).
  - Edits of a target are undo steps, so `apply_patch`, `set_params`, `set_modulation`, `set_shape`, `history` and `load_preset` are not destructive. Codex refuses tools marked destructive when no one can approve them, which is how session 1 found the first, too broad, marking.
  - `save_preset` and `render_audio` overwrite only with an explicit `overwrite: true` and are not marked destructive.
  - `openWorldHint` is false everywhere: no tool reaches the network.
- **Headless sessions live in the server process** (lost when it ends, removed after 2 hours unused, at most 16), as the specification's guidance on stateful tools suggests. Clips are files (`Documents/Zederia/Clips/Agent/<id>.json`), so a clip id survives a restarted server. Tables made by agents are saved as `user:Agent/<name>` and are also kept inside the patch of the target they are loaded into, so every instance plays them alike.
- **Analysis choices**:
  - Loudness follows ITU-R BS.1770 (K-weighting, 400 ms blocks, absolute and relative gates).
  - Pitch uses YIN on 4096-sample frames within 20 dB of the peak; the result is the median of confident frames. A spread above a semitone marks the pitch as `varies`, which turns off the harmonic measures.
  - Attack uses 1 ms windows every 0.25 ms.
  - Tag thresholds are listed in the agent guide.
  - Comparison distance = spectral + envelope/2 + |loudness|/2 (dB), similarity = 1/(1 + distance/6).
- **Loops**: a clip renders exactly its bars at its tempo. `fold` adds the tail (trimmed below -90 dBFS) onto the loop's start, wrapping if longer. `separate` writes `<name>_tail.wav`. A clip transposes by the smallest move between tonics (-6 to +5 semitones), with notes kept in range by octaves.
- **The HTTP endpoint**:
  - It streams `tools/call` in 2026-07-28 always (and any request with a progress token), so a closed stream can cancel. An SSE comment every 250 ms during long work finds a closed stream.
  - It never sends CORS headers, so a browser page cannot read its answers even from an allowed origin.
  - Without `--token` a random token is made and written to `%LOCALAPPDATA%\Zederia\mcp-http.json` (user-only).
- **Audition cap**: the processor holds an agent audition's output peak at or below the cap (-12 dBFS by default) with a gain that drops at once and recovers slowly, for the phrase and two seconds after it. It needs no allocation.
- **`ZEDERIA_AGENT_LINK=1`** switches the link on at start and keeps it on through a restored session or project. It is only for automated live-link tests; `ZEDERIA_LOCAL_DIR` and `ZEDERIA_SETTINGS_DIR` give tests their own discovery and permission folders.
- **Real-client verification ran through Codex.** Claude Code's command-line program is not signed in here, and signing in is the user's step.

## Known issues
- **The Connect panel's prompts were not run in the agents.** Their text is tested (server path, how to add it to each client, the guide), but no agent has followed one here yet. The Antigravity prompt asks the agent to find `mcp_config.json` itself because its location is unconfirmed (Antigravity is not installed).
- **Agents still save outside User Presets**: `save_preset` writes to `Presets/Agent/` or `Presets/<pack>/`, which the tree shows as folders of their own.
- **Save uses the framework's dialog** (themed by the look and feel, not the hardware finish); its Location list is editable text, so a typed folder name is made when saving.
- **Claude Code ran no session here.** Its command-line program on this machine is not signed in. `claude mcp list` connects to the server, which exercises `server/discover` at 2026-07-28 and the tool list. To run session 1 yourself: sign in with `claude` once, then in this folder run `claude -p "Make a warm, slightly detuned pluck for a house track with the zederia tools" --allowedTools mcp__zederia`.
- **The Antigravity CLI is not installed**, so its configuration path and protocol revision were not checked.
- **The live link is Windows-only** in this version (named pipes); elsewhere the server lists no live targets.
- **Interface timing tests are load-sensitive.** They passed in this milestone's first full run and failed in later runs while the desktop app used a third of the CPU (2.4 to 3.4 % against 2 %, 55 to 68 ms against 50 ms). The milestone 5 build fails them equally at the same time (Measurements), so this is the machine, not the code. Rerun `scripts/test.ps1 -Filter "[perf]"` on a quiet machine before release.
- **A patch carrying a large edited table** is sent whole over the pipe for every live edit (read and write), a few hundred kilobytes for a 64-frame table. Edits stay well under the 8 s timeout but are slower than for reference tables.
- **The audition cap** steps the gain down within one block when a peak arrives, which can click faintly. It is a safety cap, not a limiter.
- **The multi-note "noisy" mistake is fixed but other tags can mislead on phrases**: brightness and decay describe the whole render. The guide says to judge harmonic measures on a single held note.
- **Computer keys inside a host**: the plug-in hears letters only while its window has keyboard focus, and some hosts take letters first for their own typing keyboard (FL Studio's "typing keyboard to piano" sends notes to the selected channel itself). The Standalone was checked by hand; no host has been.
- **The interface's cost is near its budget** (1.6 to 2.0 % of 2 %): the dock and keyboard add widgets to every page and each repaint has a fixed cost. A slow day on this virtual machine can push the perf test over.
- **Cached surfaces use memory**: each card, knob cap and modulated knob face is an image at the display's density (about 6 MB in all at 100 %, four times that at 200 %).
- **Base size changed**: hosts that stored the editor's size from before keep it until the editor reopens; the stored percentage now applies to 1320 x 940.
- **The machine's speed varies.** It is a virtual machine; it has been seen at half speed for hours, and full benchmark runs differ by about 10 % (`full8` read 13.0 % and 14.3 % in two runs a few minutes apart). The baseline holds the slower reading. If a benchmark fails unexpectedly, check that `idle` is near its baseline before looking for a regression. The interface measurement varies more: 1.4 to 1.8 % in its own process, as `ctest` runs it, but once 2.7 % when run after the other editor tests in one process.
- **Nobody has used the interface by hand in a host.** It was checked by the editor tests (driving the controls through their mouse and keyboard handlers), offscreen snapshots, pluginval's editor tests and the Standalone's window; no screen reader was run against it (names, roles and values are tested through the accessibility handlers).
- **The undo history is not saved** with the host project; reopening a project starts a new history at "Project opened".
- **Interface cost grows with scale**: the measured 1.4 to 1.8 % is at 100 %; at 200 % the animated areas hold four times the pixels. The 3D table view's lit line and the moving markers are the main cost.
- **Texture samples are not embedded** in patches (tables edited in the workshop are); a preset that uses a user sample needs the file.
- **The graphics-card renderer** (an option in the preferences) is untested on real hardware; software rendering is the default.
- **The interface font** is the system's (Segoe UI on Windows, with fallbacks); text widths are measured, and the snapshot checks catch truncation, but another system font can change the fit.
- **A moved effect dips.** Moving an effect, or switching lane B between parallel and series, fades the moved effects out and back in over 20 ms: a long reverb or delay tail in them dips audibly (no click). Removing an effect cuts its tail with a 10 ms fade.
- **A quality change while notes play** ducks the voices for about 12 ms; effect tails continue.
- **A value changed by hand during a rack edit's 10 ms fade** can be replaced by the value the edit carried.
- **Routes onto effect parameters** are evaluated once for the rack; a per-voice source on them follows the newest voice (as the plan says). When a locked rack survives a preset change, the incoming patch's routes onto effects are dropped.
- **The Standalone's MIDI buffer is not pre-sized** by the framework, so the first block that sends MIDI out there may allocate once. The VST3 and CLAP wrappers reserve the room.
- **A voice filter still restarts when its type changes** (as in milestone 3); the Filter effect crossfades instead.
- **Clips in Transport mode** follow the host's song position block by block; a host whose position drifts against its sample count can move a note by a sample.
- **Draft is barely cheaper than Standard** in `full8` (13.7 % against 14.2 % in the last run, and slower in the baseline run, within the machine's noise): plain wavetable oscillators are never oversampled, so only warped, modulated and nonlinear paths save.
- **clap-validator warns** that 1 MB of random bytes loads as state without an error: the framework's state call has no way to report failure. The data is ignored and the state stays as it was.
- **Preset loading is dominated by wavetable builds** (about 14 ms of the 17 ms `preset_load` for a 64-frame table, single-threaded); tables already built in the session load at once.
- **Audio-rate sources act per sample only on filter cutoffs.** An oscillator's audio output or an audio-rate LFO routed anywhere else is read once per control tick (3 kHz at 48 kHz), which is too slow to be useful above a few hundred hertz. At most four such routes per filter are taken per sample; further ones fall back to the control rate.
- **Unison index** reaches only an oscillator's position, warp amounts, pan, fine and coarse; on any other destination it reads as zero.
- **MPE**: a note-off is matched by key, not by channel, so the same key held on two member channels releases the wrong one first; only the lower zone is handled. The play features (arpeggiator, chord memory, scale lock, strum, clips) send their notes on channel 1, so they do not combine with MPE; with all of them off, events pass through untouched.
- **The side-chain input of the envelope follower** exists in the engine (`ProcessArgs`) but the plug-in has no side-chain bus yet, so in a host the follower's side-chain source hears silence.
- **Routes onto a modulator's own controls and onto other routes' amounts act one control interval late** (0.33 ms at 48 kHz).
- **The defaults for decay and release curves (0.5) are audible**, so the init patch decays faster at first than it did before milestone 3.
- **Warmth is not oversampled**; at high levels and high notes its harmonics can alias. The filter response display shows the linear part of the types that have no linear response (ring modulator, sample-and-hold, grit comb).
- The Texture player reads its sample with plain interpolation and no band-limited copies, so a sample played far above its recorded pitch can alias.
- File paths with characters outside the system's code page are not handled (table, texture, tuning, content and program files).
- Hard sync and other phase warps rely on oversampling alone; their aliasing falls with the quality mode but is audible in Draft at high notes.
- Nobody has listened to any of it: the effects, the arpeggios and the reference patches were judged by measurements of their renders.
- The Debug suite takes about three minutes (182 s); Release takes about half a minute.
- FL Studio per-user plug-in folder scanning unconfirmed (no host has loaded the plug-in).
- Inno Setup not installed (needed by milestone 7). JUCE licence choice is open.

## Next
- Milestone 7 (factory content and release). The agent tools are ready for designing the factory presets: brief, edit, render, analysis, adjust, save; loudness and naming conventions are in `docs/AGENT_GUIDE.md`. The installer must place `zederia-mcp.exe` in `%LOCALAPPDATA%\Programs\Zederia\` (the AGENT page's snippets point there). The free presets go into the category folders the library makes (`808`, `Bass`, `Synth`, `Lead`, `Pad`, `Pluck`, `Keys`, `Arp`, `FX`); decide whether the installer copies them into `Documents\Zederia\Presets` or keeps them read-only in the factory folder under the same folder names (the tree shows both in the same folders).
- To run the example sessions from Claude Code as well: sign in its command-line program (`claude`), then use the `.mcp.json` or `claude mcp add` snippet from the AGENT page (already registered for this folder).
