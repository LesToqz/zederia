# Zederia — Build Plan

Version 1.0 · 10 November 2026 · Companion file: `BUILD.md` (the staged build prompts)

Zederia is a wavetable synthesizer plug-in written in C++. It pairs a fast, visual sound-design workflow with a local bridge that lets AI coding agents design sounds, build presets, and render melodic and harmonic samples on the instrument.

## 0. How to use this document

- This file is the specification. `BUILD.md` contains the prompts that build it, one milestone per session; each prompt points at section numbers here.
- "Must" marks something a milestone is accepted against. Where a mechanism is described as "suggested", the builder may pick another that meets the stated measurable outcome, and records the choice in `docs/STATUS.md`.
- Numbers (ranges, counts, budgets) are defaults chosen so nobody has to guess. Change them only with a recorded reason.

---

## 1. Product definition

### 1.1 Pillars

1. **Sound without waste.** Alias-controlled oscillators, filters with character, and CPU budgets enforced by benchmarks rather than hope.
2. **See what you hear.** Every source, route and modulation is visible and editable where it lives.
3. **Nothing is a dead end.** Full undo, A/B, locks, copy and paste for every module and shape, and an open patch format.
4. **Agent-native.** Everything a person can do in the interface, an agent can do through a documented, local, safe protocol, and agents get "ears" through rendering and analysis.
5. **Its own identity.** Original DSP, original look, original factory content.

### 1.2 Audience

Producers and sound designers; preset and sample-pack makers; developers and AI agents that automate sound creation.

### 1.3 Scope of version 1

| In version 1 | Deliberately later |
|---|---|
| Three wavetable oscillators, sub, sample-based texture layer | Granular, spectral and multisample engines |
| Two voice filters with flexible routing | Convolution reverb, frequency shifter, band and mid/side splitters |
| 4 envelopes, 8 LFOs, 8 macros, 64-route matrix, MPE | Multi-core voice rendering |
| Effects rack with three lanes | AAX format, Linux builds |
| Arpeggiator, chord tools, clip player, MIDI out | Online accounts, store, cloud sync |
| Wavetable workshop (draw, harmonic edit, formula, import, export) | A chat window inside the plug-in (agents connect from outside) |
| Library with tags, search, bulk editing | |
| Agent bridge (MCP server, live link, render and analysis) | |

### 1.4 Platforms and formats

Windows 10/11 x64 first: VST3, Standalone, CLAP. macOS (AU, VST3, CLAP, universal binary) is the first port after version 1, so the core must stay portable: no platform calls outside clearly isolated files.

Zederia works fully offline. It has no activation step and sends no telemetry.

---

## 2. Design drivers from user research

Before fixing the feature list we reviewed public feedback from producers about the wavetable instruments they use today: forum threads, community and store reviews, vendor support forums and long-term write-ups, spanning roughly ten years. The same themes repeat. Each one maps to a concrete response in this plan.

| # | What producers report | Zederia's response | Section |
|---|---|---|---|
| 1 | Heavy unison and polyphony overload the CPU; requests for a draft mode; some patches unusable on recent hardware | Benchmarked CPU budgets, SIMD unison, Draft/Standard/High/Ultra quality, separate offline quality | 3.12, 5.7, 5.8 |
| 2 | CPU rises when the plug-in window is open; GPU-accelerated interfaces misbehave on some systems | Interface frame cap, full pause when hidden, software rendering by default, no GPU requirement | 3.13 |
| 3 | Sound described as sterile, cold or clinical; warmth has to be added with other plug-ins | Character section (drift, per-note variance, warmth), nonlinear filters, analog-style oscillator engine | 3.2, 3.5, 3.6 |
| 4 | Filters considered the weak point; only one voice filter, or two locked in series; want parallel and per-source routing | Two voice filters, serial-to-parallel blend, per-source sends, nonlinear models | 3.5 |
| 5 | Audible stepping between wavetable frames under slow modulation; pops on some tables | Continuous real-time frame morphing with smoothing; click-free table swaps | 3.3, 5.7 |
| 6 | No undo for years; the single most repeated request | Unlimited named undo/redo covering parameters, routing, shapes, table edits and preset loads | 3.11 |
| 7 | One fixed effects chain with one of each effect; want more buses and parallel processing; reverb and chorus judged weak | Three-lane rack, any effect any number of times, a serious reverb and chorus | 3.8 |
| 8 | No built-in arpeggiator or sequencer; MIDI is not passed through to other plug-ins | Play section: arpeggiator, chords, clip player, MIDI out and thru | 3.9 |
| 9 | Envelope times only in milliseconds; envelope and LFO shapes cannot be copied | Type values as note lengths; copy and paste for every shape and module | 3.7, 3.13 |
| 10 | Too few modulators and macros; matrix lives on a separate page; hard to see what modulates what; want drawable curves per route | 8 macros, 4 envelopes with multi-segment mode, 8 LFOs, inline modulation lists, mod map, per-route curves | 3.7 |
| 11 | Preset browsers lack tags, search, favourites and bulk tagging; importing banks is awkward; factory sounds do not show the range | Library with facets, search, bulk edit, pack import; curated, varied factory bank | 3.11, 7 |
| 12 | Presets break when shared because sample content is not stored with them | Samples and tables are embedded or collected with the preset | 3.2, 5.6 |
| 13 | Useful features are hidden behind modifier keys or only explained in forums; unlabeled controls; intimidating for beginners | Help strip, command palette, labels on everything, Simple view | 3.13 |
| 14 | Small or fixed-size interfaces on high-resolution displays | Free vector scaling from 70% to 300% | 3.13 |
| 15 | Audio-to-wavetable import is unclear and results disappoint; want to erase or paint parts of the spectrum | Import wizard with preview and cycle markers; spectral paint and erase view | 3.10 |
| 16 | Scripts and external tools can change amounts but not routing or tables; preset files are opaque | Open JSON patch format; complete agent interface | 4, 5.6 |
| 17 | Frequency modulation between sources is restricted (fixed pairs, octave-only pitch for some sources) | Any source can modulate any oscillator, free ratios | 3.2 |
| 18 | Master tuning range too narrow; micro-tuning only through files | ±100 cents with 0.1-cent steps, reference pitch, tuning files | 3.6 |
| 19 | Want an audio-follower as a modulation source | Envelope follower source | 3.7 |
| 20 | Activation gets in the way on offline studio machines | No activation, fully offline | 1.4 |
| 21 | No CLAP build | CLAP is a target format | 1.4, 5.1 |
| 22 | Wavetable editors that lag while editing | Interaction budget for the workshop | 3.13, 5.8 |

Working principles that follow from this research:

- One gesture to connect a modulation, one glance to see it, one click to mute it.
- Never make the user leave the page to finish a thought: amounts, curves and sources are editable inline.
- Any number can be typed, with units.
- Expensive features say so, and there is always a cheaper mode.
- Everything that changes sound can be undone.

---

## 3. Feature specification

### 3.1 Signal flow

```
 Osc 1 ──┐
 Osc 2 ──┤                        ┌── Filter 1 ──┐
 Osc 3 ──┼── per-source sends ────┤   serial ↔   ├── amp (Env 1) ── pan ──► voice sum
 Sub   ──┤   (F1 / F2 balance,    └── Filter 2 ──┘       parallel blend
 Texture ┘    Direct amount)

 voice sum ──► Lane A ──┐
           ──► Lane B ──┼──► Master lane ──► output
 direct ────────────────┘
```

Everything left of "voice sum" runs per voice. The effects lanes run once on the summed signal.

### 3.2 Sound sources

**Oscillators 1–3** (identical). Each has an engine switch:

- *Wavetable* — plays a table of up to 256 single-cycle frames (3.3).
- *Classic* — a low-cost virtual-analog engine: saw, square, triangle and sine with continuous shape morph and pulse width, band-limited by polynomial step correction. It exists for warm, simple sounds at a fraction of the CPU.

| Control | Range / notes |
|---|---|
| On | Power switch; an off oscillator costs nothing |
| Table | Name, previous/next, opens the table browser; modified tables are flagged until saved |
| Position | 0–100%, continuous across frames |
| Octave / Semi / Fine | ±4 oct, ±12 st, ±100 cents |
| Coarse | Continuous ±64 st, intended as a modulation destination |
| Level, Pan | Level in dB, pan bipolar |
| Phase, Phase random | Start phase 0–360° or "continue from last note"; randomness 0–100% |
| Warp A, Warp B | Two warp slots in series, each with mode and amount (3.3) |
| Unison | See 3.4 |
| Key tracking | On, or fixed pitch for drones, percussion and modulator duty |
| Sends | Filter 1 / Filter 2 balance plus Direct amount |

**Cross-modulation.** In a warp slot an oscillator may take any other source (Osc 1–3, Sub, Texture) as a modulator for phase modulation, amplitude modulation or ring modulation. The modulator can be silent in the mix. Modulators are evaluated first; self-modulation and loops use a one-sample delay.

**Sub.** Shapes: sine, triangle, saw, square, 25% pulse, rounded square. Octave −4…0 relative to the note, fine tune, level, pan, "direct" switch (skips the filters). Usable as a modulator at any ratio.

**Texture.** A stereo sample player for noise beds and attack layers: one-shot or loop (uses loop points stored in the file when present), start position, random start, key tracking on/off, pitch ±48 st with fine tune, level, pan, direct switch. Factory textures are synthesized by build scripts (white, pink, brown, hiss, crackle, breath, wind, digital grit, short transients). User samples are embedded in the preset on save (default) or collected beside it.

### 3.3 Wavetables and warping

- A frame is one cycle of 2048 samples. A table holds 1–256 frames.
- A table is stored as its key frames plus a morph mode (None, Crossfade, Spectral, Spectral with aligned phases). The full set of 256 positions is generated when the table loads, so files stay small.
- Position is continuous. The oscillator interpolates between neighbouring frames in real time, and position changes are smoothed, so slow sweeps never step. A per-table "Stepped" flag opts out for deliberate staircases.
- Interchange: tables import and export as WAV with frames laid end to end (2048 samples per frame, mono, 32-bit float preferred, 16/24-bit accepted) plus a Zederia metadata chunk. Other frame sizes are detected or chosen on import.

**Warp modes** (each slot: mode + amount; the 2D display shows the warped cycle live)

| Mode | What it does |
|---|---|
| Sync | Plays a faster inner cycle that restarts every period; classic bright formant sweep |
| Soft sync | Same, with an amplitude window that removes the restart edge |
| Bend + / − / ± | Curves phase so the wave pinches toward the middle, toward the edges, or either way around a neutral centre |
| Pulse width | Squeezes the cycle into part of the period |
| Skew + / − / ± | Leans the whole cycle left or right |
| Mirror | Plays the cycle forward then backward within one period |
| Flip | Inverts polarity after a movable point in the cycle |
| Quantize | Steps the phase: pitch-tracking sample-rate reduction |
| Curve (4 variants) | User-drawn phase transfer curve applied to the full cycle, to each half, blended with a sine curve, or four times per cycle |
| Fold | Wave-folds the amplitude |
| Drive | Soft saturation per oscillator |
| PM / AM / RM from source | Phase, amplitude or ring modulation by another source |

### 3.4 Unison (per oscillator)

Voices 1–16 · Detune 0–100% of Range · Range 0–48 st (default 2) · Blend (centre versus side voices) · Stereo width · Position spread and Warp spread across voices · Spread law: Linear, Ensemble (denser near centre), Exponential, Inverse, Random per note · Stack: Off, alternate +12, +12/+24, fifths, fifths and octaves, centre −12, centre −24 · Start phase: Free, Random, Consistent (a stored set of random phases, so every note attacks the same way without the swept "zap" of zeroed phases).

Level is compensated so changing the voice count keeps loudness within ±1.5 dB. The voice-count box warns by colour as cost rises.

### 3.5 Filters

Two slots per voice. Controls: On, Type, Cutoff (8 Hz–22 kHz), Resonance, Drive, Character (meaning depends on type), Stereo offset (splits left and right cutoff), Key tracking 0–100%, and Mix that can switch to Level (output trim with no dry signal). The response display is live, shows modulation, and toggles to phase.

Routing: a Serial↔Parallel blend between the slots, plus per-source sends (3.2).

| Family | Types | Character knob |
|---|---|---|
| State-variable | Low-pass and high-pass 6/12/18/24 dB, band-pass, notch, peak 12/24 dB | Body: resonance bass compensation and saturation |
| Ladder | Low-pass 6/12/18/24 dB, nonlinear, self-oscillating | Heat |
| Dual | Pairs of state-variable filters with linked resonance (low+high, low+band, low+peak, low+notch, high+band, high+peak, high+notch, band+peak, band+notch, peak+peak, peak+notch, notch+notch) | Second cutoff offset |
| Morph | Low↔band↔high, low↔peak↔high, low↔notch↔high, band↔peak↔notch, 12/24 dB | Morph position |
| Comb, flange, phase | Positive and negative feedback, with low-pass, high-pass or band damping in the loop | Damping cutoff or width |
| Formant | Vowel morph, three voicings | Formant shift |
| Shelf and bell | Low shelf, high shelf, bell | Gain |
| Special | Ring modulator, sample-and-hold, all-pass cluster, diffuser, band-reject, Howl (high-feedback distorting low/band-pass), Grit comb | Type-specific |

Filters are a stated priority: nonlinear types are oversampled in Standard quality and above, resonance stays stable under audio-rate modulation, and each family is tested against its analytic response where one exists.

### 3.6 Voice, tuning and character

- Polyphony 1–64 (default 16). Modes: Poly, Mono, Legato. Stealing: oldest, quietest, lowest-priority release. Mono note priority: last, low, high. Live voice count on screen.
- Glide: time 0–5 s, curve, Always or legato-only, Scaled (time follows interval size).
- Pitch bend range up and down, 0–48 st each. MPE: per-note bend (default 48 st), pressure, slide.
- Tuning: master ±100 cents in 0.1-cent steps, reference A = 400–480 Hz, Scala (`.scl`/`.kbm`) and `.tun` files, optional tuning lock across preset changes.
- Character: Drift (slow per-voice pitch wander), Variance (per-note random offsets for cutoff, envelope times, pan, level; each with its own amount), Warmth (gentle pre-filter saturation with a high-frequency tilt), Noise floor.
- Amp: Env 1 drives level; velocity sensitivity; master volume; DC blocker; optional output soft-clip or limiter.

### 3.7 Modulation

**Envelopes ×4.** Env 1 is the amp envelope. Standard mode: Delay, Attack, Hold, Decay, Sustain, Release with a curve per segment; 0–30 s. Times can be typed as milliseconds, seconds or note lengths (`1/8`, `1/4T`, `3/16`), and a tempo-linked value stays linked. Env 2–4 can switch to multi-segment mode: up to 32 points, curves, a sustain point and a loop region.

**LFOs ×8.**
- Shape editor: up to 64 points with curve handles; grid with separate X and Y divisions; step drawing; multi-select and proportional move; shape library; copy and paste.
- Modes: Free (ignores notes), Retrigger, One-shot (acts as an envelope), Loop-from-point.
- Rate in Hz (0.01 Hz up to 2 kHz with audio-rate mode on) or tempo-synced (8 bars to 1/128, triplet and dotted). "Anchor" keeps a synced LFO locked to song position when its rate changes.
- Phase, Delay, Rise (fade-in), Smooth, unipolar/bipolar, per-voice or global, optional key-tracked rate.

**Flux ×2** (random and chaos generators): orbit (smooth, loosely periodic), burst (irregular restarts), stepped sample-and-hold, smooth noise. Rate free or synced, per-voice or global.

**Macros ×8.** Named, optional bipolar, visible on every page, assignable by drag.

**Other sources.** Velocity, release velocity, note number, pitch bend, mod wheel, channel and polyphonic aftertouch, MPE pressure and slide, expression, breath, sustain pedal, any learned MIDI CC, Random 1/2 (new value per note), Alternate (flips each note), voice index, unison index, constant, audio-rate outputs of Osc 1–3/Sub/Texture, envelope follower (on the instrument's own pre-effects signal or a side-chain input), host bar phase.

**Matrix: 64 routes.** Each route has Source, optional Via source (scales the route; can be inverted), Amount ±100%, Polarity (unipolar/bipolar), Curve (linear, exponential, logarithmic, S, or drawn), Destination, and Enabled / Mute / Solo. Any continuous parameter is a destination, including effect parameters, other modulators' rates and depths, and other routes' amounts. Routes are polyphonic when both ends are per-voice.

**Assigning and seeing modulation.**
- Drag a source chip onto a control; or right-click a control → Add modulation; or use the MOD page. All three edit the same route list.
- Each modulated control shows one depth arc per source (the selected source highlighted) and a moving marker for its live value. Alt-drag changes the selected source's depth; Shift+Alt-click flips polarity.
- Hover a source: its destinations light up. Hover a control: a small list of its sources appears with editable amounts, mute and remove.
- A "mod map" overlay draws all active connections at once. The MOD page lists routes with search, sort and bulk source replacement.

### 3.8 Effects

A rack with three lanes — A, B and Master — of up to 8 slots each. Any effect type can go in any slot, any number of times. Slots are dragged to reorder or move between lanes. Lane B runs in parallel with A or in series after it. Filter outputs and the direct signal are balanced between A and B; both feed Master.

Effects run on the summed signal. A per-voice source assigned to an effect parameter follows the newest voice; the interface says so in the tooltip.

| Effect | Controls and notes |
|---|---|
| Drive | Modes: Tube, Soft clip, Hard clip, Diode A, Diode B, Linear fold, Sine fold, Rectify, Asymmetric, Bit crush, Downsample, Sine shaper, Fuzz, Custom curve, Dual curve (two drawn transfer curves with a morph, symmetric or asymmetric). Pre/post filter (low↔band↔high morph, frequency, Q). Oversampled according to quality |
| Compressor | Threshold, ratio (limit at maximum), attack, release, make-up, mix. Multiband mode: three bands of upward and downward compression with per-band threshold and amount, adjustable crossovers |
| EQ | Four bands: low (shelf, bell or high-pass), two bells, high (shelf, bell or low-pass); spectrum display |
| Filter | Every voice filter type, in stereo, with all controls |
| Chorus | 2–6 voices, rate (Hz or sync), two delay times, depth, feedback, low- or high-pass on the wet signal, mix |
| Flanger | Rate (Hz or sync), depth, feedback ±, stereo phase, mix |
| Phaser | 2–12 stages, rate, depth, centre frequency, feedback, stereo phase, mix |
| Delay | Modes: Dual, Ping-pong, Pre-tap. Left/right times in ms or synced with fine offset (triplet and dotted detents), link, feedback, feedback band (centre and width), saturation, modulation, ducking, mix |
| Reverb | Plate, Hall, Room. Size, decay, pre-delay, low cut, high cut, damping, modulation, width, freeze, mix |
| Swarm | Micro-detune ensemble of 1–7 voices: rate, detune, restart on note, mix |
| Widener | Four-tap decorrelation: size, mix; mono-compatibility indicator |
| Utility | Gain, pan, width, mono below a frequency, polarity, DC filter |
| Limiter | Ceiling, release |

Rack housekeeping: per-slot power, mix and preset menu; rack and single-effect presets; "lock rack" across preset changes; each slot goes idle once its tail has decayed; a global **FX off** switch in the top bar.

### 3.9 Play

- **Arpeggiator.** Modes: Up, Down, Up-down (with or without repeated ends), Random, Walk, As played, Chord. Octaves 1–4, synced rate, gate, swing, latch, restart. A step lane of 1–32 steps, each with on/tie/rest, velocity, transpose and ratchet. Twelve pattern slots per preset.
- **Chords.** Chord memory (a stored chord from one key), scale lock (key and scale; snap or filter), strum time and direction.
- **Clip player.** Eight clip slots per preset. A clip is a note list with length in bars, tempo and key hints. Triggers: host transport, or a key (the clip transposes to the played note), with latch and loop. Clips import by dropping a `.mid` file and export by dragging out to the host. Version 1 has a piano-roll view with basic edits (move, length, velocity, quantize, delete).
- **MIDI.** Notes generated by the arpeggiator, chords and clips are sent to the host; MIDI thru is optional. MIDI learn on every control; CC maps save and load; program change selects presets from a list.

### 3.10 Wavetable workshop

- **Frames strip.** Thumbnails of key frames; click to select, drag to reorder, range and multi-select, insert, duplicate, delete, thin (keep every Nth), crop to selection.
- **Wave view.** Draw with a grid and snapping. Tools: flat, ramp up, ramp down, sine, half-sine up and down, quarter curves, straight and curved interpolation between grid points, nudge, add noise.
- **Harmonic view.** Bars for magnitude and for phase of each harmonic. Operations: clear all, clear above or below a harmonic, fill as saw series, randomize the lowest N (full or half amplitude), randomize all, random gaps, progressive fade of highs, octave up or down, odd-only and even-only drawing, snap heights to quarters, scale by harmonic number.
- **Spectral view.** Frames by harmonics as a heat map with paint, erase and smooth brushes and a harmonic lock; this is the place to remove or carve part of the spectrum across a whole table.
- **Process (one frame or all).** Normalize each or to a shared peak, remove DC, flip polarity, mirror in time, shift to zero crossing, clear, fade edges, crossfade edges, low-pass by harmonic count, remove fundamental, sample-rate reduce, split frames in two, join frame pairs, make a pulse-width sweep from one frame, align fundamental phase, copy the selected frame's spectrum or phases to all, subtract another oscillator's spectrum.
- **Morph.** Fill between key frames by crossfade or by spectral interpolation (with original phases, fundamental-aligned, or all phases zeroed); remove morph.
- **Sort.** By brightest peak, average brightness, strongest partial, number of active partials, highest active partial, fundamental strength; invert order.
- **Formula.** Generate frames from a math expression in the wave or harmonic domain (Appendix B). Factory and user formula lists.
- **Import.**
  - Single-cycle files → frames (drop several to fill several frames).
  - Pitched recording → cycle slicing: pitch-tracked (optionally snapping to zero crossings) or fixed cycle length (averaged automatically, typed in samples, or as a note name).
  - Spectral snapshots: STFT sizes 256–2048, phase handling selectable.
  - Image → table (rows become frames; brightness becomes amplitude or harmonic level).
  - Stereo files: choose left, right or mid; one gesture sends left to Osc 1 and right to Osc 2.
  - An import wizard shows the source with detected cycle markers, lets the result be auditioned before committing, suggests a method, and offers follow-ups (fade edges, normalize, trim silent frames).
- **Resynthesis.** Bake warp (render the current frame across the warp range into 256 frames), Bake patch (play one bar of the current sound and capture it as a table), Shape to wave (an LFO shape becomes a frame).
- **Export.** Whole table, selection, or a folder of single-cycle files.

Every workshop action is an undo step.

### 3.11 Library

- **Preset fields.** Name, author, pack, type, character tags, genre tags, rating 0–5, favourite, notes, dates. Presets are `.zpreset` files (5.6).
- **Browser.** Facets on the left (Type, Character, Genre, Pack, Author, Favourites, Recent, Factory/User); search box with free text and `tag:` style filters; results with sortable columns and a secondary sort; audition by an automatic phrase suited to the preset type or by playing keys; multi-select for bulk tag, rate, move and export; drop a folder or pack file to import; duplicates detected by content.
- **Module presets.** Oscillator, filter, envelope, LFO shape, effect slot, effect rack, arpeggiator pattern, clip. Copy and paste between like modules anywhere.
- **Locks.** Parameter, module, rack, polyphony and tuning locks survive preset changes.
- **Variation.** Randomize (choose scope and amount; respects locks), Mutate (small changes), Blend (build a new sound from modules of several selected presets), Init, user-defined default preset.
- **Compare.** A/B with copy between sides.
- **Undo/redo.** Unlimited within a session (bounded by memory), named steps, visible history list. Covers parameters, routing, shapes, wavetable edits, preset loads, and edits made by agents.

### 3.12 Global

- **Quality.** Draft (no oversampling, cheaper filter paths), Standard (2× for warped and nonlinear paths), High (4×), Ultra (8×). Realtime and offline quality are set separately; offline quality engages automatically when the host renders faster than real time.
- **Performance.** CPU meter (average and peak), voice count, per-section cost hints, voice limit, interface frame cap (15/30/60), FX off.
- **Preferences.** Default size, theme, knob behaviour, mouse wheel, double-click action, tooltips, default author, content folder, MPE default, automation smoothing, agent link and its permissions.

### 3.13 Interface

**Window.** Freely resizable from 70% to 300% of a 1320×820 base, aspect locked, fully vector, sharp on high-density displays.

**Top bar.** Menu · preset name with previous/next and save · undo/redo with history · A/B · quality · CPU · FX off · agent indicator · output meter · master volume. A macro strip can stay visible on every page.

**Pages.** SYNTH · FX · MOD · TABLE · PLAY · LIBRARY · AGENT.

**SYNTH page.**
- Left: source cards (Osc 1–3, Sub, Texture), collapsible, each with a small live wave display.
- Centre: the **Stage** — one large display that follows focus: the 3D table or 2D warped cycle for an oscillator, the response curve for a filter, the editor for an envelope or LFO, or the signal map.
- Right: Filter 1, Filter 2, routing, voice, glide, character.
- Bottom dock: modulators (Env 1–4, LFO 1–8, Flux, Macros) with drag chips; collapsible keyboard with wheels.
- **Signal map.** A compact diagram of the current routing with levels; click a node to jump to its module.

**Interaction rules.**
- Vertical drag; Shift for fine; double-click to reset or to type (preference); Ctrl-click resets.
- Right-click any control: type value, MIDI learn, show automation, lock, copy/paste, add modulation, bypass or remove modulation.
- Typed values accept units: Hz, kHz, ms, s, st, cents, dB, %, note lengths, note names.
- Command palette (Ctrl+K) finds any parameter, action or preset.
- Help strip at the bottom describes whatever is under the pointer; F1 pins it. Every control has a visible label. A Simple view hides advanced controls.

**Accessibility.** Keyboard access to every control; names and values exposed to screen readers; a high-contrast theme; modulation colours chosen to survive colour-blindness and backed by position cues; no text below 11 px at 100%.

**Look.** Original design. Default theme "Graphite": near-black neutral surfaces, one accent hue per module family (sources, filters, modulators, effects), thin luminous display lines, generous spacing. Also "Paper" (light) and "Contrast". Themes are token files. The layout and artwork must not imitate any existing product.

**Interface performance.** Idle repaint cost is near zero; animated displays respect the frame cap; all animation stops when the window is hidden; rendering does not depend on a GPU.

---

## 4. Agent integration

### 4.1 Purpose

Let an AI coding agent — Claude Code, Codex, the Antigravity CLI (`agy`), or any client that speaks the Model Context Protocol — connect to Zederia and:

- design a sound from a text brief, iterating by listening through analysis;
- build and tag presets and whole preset packs;
- create wavetables from harmonic recipes, formulas or audio;
- write melodies and chord progressions and render them as loop samples and MIDI files;
- work on the instance the user is hearing in their host, or with no host running at all.

### 4.2 Architecture

```
 agent client ──(MCP: stdio, or HTTP on 127.0.0.1)──► zederia-mcp.exe
                                                        │
                        ┌───────────────────────────────┼──────────────────────────┐
                        ▼                                                          ▼
              headless sessions                                          live link (local pipe)
        (the same engine library, in-process:                    to Zederia instances running in a host
         edit, render, analyze — no host needed)                 (edits appear on screen and are audible)
```

- `zederia-mcp` is a small executable. It owns all protocol handling, so protocol revisions never require touching the plug-in binary and no network port is opened inside the host process.
- It links `zederia_core`, the same engine the plug-in uses. Headless results therefore match the plug-in exactly.
- Each plug-in instance with Agent Link enabled opens a local pipe and writes a small discovery file. The server lists these as *live targets* and forwards commands. Live edits are queued to the instance and applied at block boundaries.
- A *target* is an explicit handle (`session:…` or `live:…`) passed as an ordinary tool argument. The server holds no hidden per-connection state.

### 4.3 Protocol

- **Transports.** stdio (the default; all three named clients launch servers this way) and Streamable HTTP bound to `127.0.0.1` behind a flag.
- **Revisions.** The server must interoperate with both generations of the protocol in use today:
  - the handshake generation (`initialize` first; revisions 2025-06-18 and 2025-11-25);
  - the stateless generation (revision 2026-07-28): `server/discover`, protocol version and client capabilities carried in each request's `_meta`, a required `resultType` on every result, no protocol sessions, cache hints (`ttlMs`, `cacheScope`) on list results, deterministic tool order, and the `MCP-Protocol-Version`, `Mcp-Method` and `Mcp-Name` headers on HTTP.
- The builder must read the current specification when implementing this part and follow it over this summary.
- **Results.** Every tool returns a short text summary plus `structuredContent` that validates against the tool's `outputSchema`. Audio is returned as file paths and resource links, never inline. Spectrogram images are returned as image content on request.
- **Long operations.** Renders report progress when the client asks for it and honour cancellation.
- **Annotations.** Each tool declares whether it is read-only, destructive, and idempotent.

### 4.4 Tool catalogue

| Tool | Purpose | Key inputs | Returns |
|---|---|---|---|
| `get_status` | Server version, permissions, live targets' tempo, meters and CPU | — | status object |
| `list_targets` | Headless sessions and live instances | — | targets with names and kinds |
| `open_session` | Start a headless session | `from`: init, preset id or patch | `target` handle |
| `close_session` | End a headless session | `target` | — |
| `describe_synth` | Modules, parameters (id, range, unit, default, meaning), modulation sources and destinations, enumerations | optional module filter | schema slice |
| `get_patch` | Current patch | `target`; full or difference from init | patch JSON |
| `apply_patch` | Replace or merge patch content as one transaction | `target`, `patch`, `mode`, `dry_run`, `label` | applied diff, or validation errors with paths |
| `set_params` | Small parameter edits | `target`, list of id/value | values after clamping |
| `set_modulation` | Add, update or remove routes | `target`, routes | route list |
| `set_shape` | Set an LFO, multi-segment envelope, warp curve or drive curve | `target`, owner, points | normalized shape |
| `history` | List, undo, redo, or revert to a step | `target`, action | history |
| `create_wavetable` | Make a table from harmonics, a formula, raw frames, basic shapes, an audio file or an image | spec, optional target and oscillator | table id, statistics, optional thumbnail |
| `edit_wavetable` | Process, morph, sort and frame operations | table id, operations | statistics |
| `list_wavetables` | Browse tables | search, tags | entries |
| `list_presets` | Browse presets | search, tags, type | entries |
| `load_preset` | Load into a target | `target`, preset id | patch summary |
| `save_preset` | Save a target's patch with metadata | `target`, name, tags, pack | preset id, path |
| `tag_presets` | Bulk edit metadata | ids, changes | count |
| `render_audio` | Render notes or a clip offline and analyze the result | `target`, notes or clip, tempo, length, tail, quality, output options | file path, analysis (Appendix C), optional spectrogram |
| `analyze_audio` | Analyze any audio file | path | analysis |
| `compare_audio` | Distance between two files | paths | spectral, envelope and loudness differences |
| `audition` | Play notes or a clip through a live instance | live `target`, notes or clip | — |
| `theory` | Resolve chord symbols and scales, quantize to a scale, voice-lead a progression | request | notes |
| `write_clip` | Create or replace a clip (notes, length, tempo, key) | clip spec | clip id, validation notes |
| `get_clip` | Read a clip | clip id | clip |
| `export_pack` | Batch-render presets × clips × keys into a named folder | lists, naming rule | file list |

**Resources.** `zederia://guide/agent` (how to work with the instrument: parameter semantics, recipes, the design loop, loudness and naming conventions), `zederia://schema/patch`, `zederia://schema/params`, `zederia://library/presets/{id}`, `zederia://library/wavetables/{id}`, `zederia://renders/{id}`.

**Prompts.** `design_sound`, `match_reference`, `write_progression_loop`, `write_melody_loop`, `build_pack`.

### 4.5 Patch model

One JSON document describes a whole sound. It is the preset file, the plug-in state and the agent's working material. A trimmed example:

```json
{
  "format": "zederia.patch",
  "version": 1,
  "meta": { "name": "Glass Pluck", "author": "", "type": "pluck", "tags": ["bright", "short"] },
  "global": { "mode": "poly", "polyphony": 16, "glide_ms": 0, "bend_range": [2, 2], "master_db": -6.0 },
  "osc1": {
    "on": true, "engine": "wavetable",
    "table": { "ref": "factory:Basics/Saw Morph" },
    "position": 0.25, "level_db": -3.0,
    "unison": { "voices": 5, "detune": 0.18, "blend": 0.7, "width": 0.8 },
    "warp_a": { "mode": "bend_plus", "amount": 0.3 },
    "sends": { "filter_balance": 0.0, "direct": 0.0 }
  },
  "filter1": { "on": true, "type": "ladder_lp24", "cutoff_hz": 1800, "resonance": 0.35, "drive": 0.2, "keytrack": 0.5 },
  "env1": { "attack_ms": 1, "decay_ms": 420, "sustain": 0.0, "release_ms": 180 },
  "env2": { "attack_ms": 0, "decay_ms": 260, "sustain": 0.0, "release_ms": 120 },
  "mod": [
    { "source": "env2", "dest": "filter1.cutoff", "amount": 0.6, "polarity": "uni" },
    { "source": "lfo1", "via": "modwheel", "dest": "osc1.position", "amount": 0.4, "polarity": "bi" }
  ],
  "fx": { "a": [ { "type": "chorus", "mix": 0.3 } ], "b": [], "master": [ { "type": "reverb", "mix": 0.18 } ] },
  "macros": [ { "name": "Bright", "value": 0.0 } ]
}
```

Rules: omitted fields take defaults; unknown fields are reported, not silently dropped; values are clamped and the clamped value is returned; validation errors carry a JSON-pointer path and a plain-language fix; tables generated from a recipe store the recipe as well as the frames.

### 4.6 Hearing: render and analysis

Agents cannot listen, so every render returns measurements in musical terms (Appendix C): loudness, peak, attack time, tail length, brightness, noisiness, pitch and stability, band balance, stereo width, amount and rate of movement, plus plain tags such as "bright", "plucky", "wide", "evolving". `compare_audio` scores a render against a reference file so an agent can close in on a target. A mel spectrogram image is available for clients that can see.

The design loop the guide teaches: read the brief → pick a starting preset or init → edit → render a short phrase → read the analysis → adjust → save with tags.

### 4.7 Music: clips and samples

- The agent writes notes; the server keeps them honest. `theory` turns chord symbols and scale names into pitches, voice-leads progressions and snaps notes to a key. `write_clip` validates range, overlaps, key membership and bar length and reports problems.
- `render_audio` with a clip produces a loop-ready WAV plus the matching `.mid`. Tail handling: cut, separate tail file, or fold the tail into the loop start.
- `export_pack` renders a matrix of presets, clips and keys with names such as `ZED_chords_GlassPluck_Fmin_124bpm.wav`, and can also render one-shots and multi-sampled notes.
- Clips written by agents load into the clip player, so a user can trigger them from the keyboard in their host.

### 4.8 Safety

- **Local only.** stdio by default. The HTTP endpoint binds to 127.0.0.1, validates `Origin`, and requires a bearer token.
- **Opt-in for live control.** Agent Link is off by default per instance. When on, the top bar shows an indicator and the AGENT page lists connected clients and every action.
- **Permissions.** Read patch (on), edit patch (on), write inside Zederia's own folders (on), write elsewhere (off; needs an allow-listed folder), audition through the user's speakers (on, level-capped), read samples from disk (allow-listed folders).
- **File sandbox.** Paths are canonicalized; traversal is rejected; only allow-listed roots and known file types.
- **Limits.** Render length per call, concurrent sessions, table size, clip size, request size and formula complexity are capped.
- **No execution.** No shell, no scripts, no outbound network. The formula language is pure math.
- **Untrusted text.** Names, notes and tags read from files are returned as data. Tool descriptions say so.
- **Reversible.** Each mutating call is one undo step labelled with the client name; "Revert agent session" restores the state from before the agent connected.
- **Pipe security.** The live-link pipe is restricted to the current user, and each instance issues a random token in a user-only discovery file.

### 4.9 Client setup

Installed tools live in `%LOCALAPPDATA%\Programs\Zederia\`. The AGENT page has copy buttons for each snippet.

Claude Code — a `.mcp.json` in the project, or `claude mcp add`:

```json
{ "mcpServers": { "zederia": { "command": "C:/Users/<you>/AppData/Local/Programs/Zederia/zederia-mcp.exe", "args": ["--stdio"] } } }
```

Codex — `~/.codex/config.toml`:

```toml
[mcp_servers.zederia]
command = "C:/Users/<you>/AppData/Local/Programs/Zederia/zederia-mcp.exe"
args = ["--stdio"]
```

Antigravity CLI (`agy`) — an `mcp_config.json` with the same `mcpServers` object as above. The file's location differs between versions (a global file under the user profile, or `.agents/mcp_config.json` in the workspace); confirm with `/mcp` inside the CLI.

### 4.10 Example sessions

1. *"Make a warm, slightly detuned pluck for a house track."* → `open_session` → `describe_synth` → `apply_patch` → `render_audio` (four notes) → analysis says attack 3 ms, bright, narrow → raise unison width, lower cutoff, add drift → render again → `save_preset` with tags.
2. *"Match this reference one-shot."* → `analyze_audio` on the file → design → `compare_audio` after each change until the distance stops improving.
3. *"Eight bars of chords in F minor at 124 BPM and a melody over it."* → `theory` for the progression → `write_clip` ×2 → `render_audio` per clip with two presets → loop WAVs and MIDI files in the renders folder.
4. *"Build a 20-preset pack of basses."* → loop of design, render, analyze, save → `tag_presets` → `export_pack` with one-shots at three pitches.

---

## 5. Technical architecture

### 5.1 Stack

| Concern | Choice | Notes |
|---|---|---|
| Language | C++20 | |
| Build | CMake ≥ 3.25 with presets, Ninja | |
| Compiler | MSVC v143 or newer; clang-cl optional | |
| Plug-in framework | JUCE, newest stable release pinned at setup | Dual-licensed (AGPLv3 or commercial tiers). The licence choice is the owner's decision and must be made before distribution |
| VST3 | SDK bundled with the framework | MIT-licensed since SDK 3.8 |
| CLAP | Native framework support if the pinned release has it, otherwise the community extensions | |
| JSON | nlohmann/json | MIT |
| Local HTTP endpoint | cpp-httplib | MIT |
| Tests | Catch2 v3 | BSL-1.0 |
| Validation | pluginval; clap-validator | Used as tools, not linked |
| FFT, audio and MIDI file I/O, zlib | Framework non-GUI modules | |
| SIMD | A thin wrapper over intrinsics (SSE2 baseline, AVX2 runtime dispatch, NEON), always with a scalar reference path | |

No further dependencies without a recorded reason.

### 5.2 Repository layout

```
zederia/
  CMakeLists.txt  CMakePresets.json  cmake/
  core/      zederia_core — engine, patch model, wavetable toolkit, renderer, analysis, music tools (no GUI code)
  plugin/    the plug-in: processor, editor, interface components, themes
  link/      local pipe protocol shared by plug-in and server
  mcp/       zederia-mcp — protocol layer, tool handlers, resources, prompts
  cli/       zederia-cli — render, analyze, table, preset, bench, ui-snapshot
  content/   recipes for factory tables and textures, factory presets, shapes, clips
  tests/     unit, integration, golden audio, protocol conformance, benchmarks
  scripts/   build, test, bench, install, packaging
  docs/      STATUS, ENVIRONMENT, ARCHITECTURE, PARAMETERS, user manual, agent guide
```

### 5.3 Threads and real-time rules

| Thread | Does | Must never |
|---|---|---|
| Audio | Renders blocks, applies queued edits at block start | Allocate, lock, wait, touch files, log, or free memory |
| Message | Interface, undo, preset I/O orchestration | Block on the audio thread |
| Workers | Table builds, imports, analysis, offline renders, library scan | Touch live engine state directly |
| Link | Pipe I/O for agent commands | Apply edits directly |

- Edits travel to the audio thread through lock-free queues. Large immutable data (tables, shapes, samples, effect graphs) is built off-thread and swapped in by pointer; the old object is retired on a non-audio thread.
- Flush-to-zero and denormals-are-zero are set in the audio callback.
- A debug-build guard marks the audio callback and fails tests on any allocation or lock inside it.

### 5.4 Engine structure

- Control-rate modulation every 16 samples at the base rate with per-sample interpolation for pitch, cutoff and amplitude; audio-rate paths for source-to-source modulation.
- Voices are processed in structure-of-arrays form so unison voices fill SIMD lanes.
- Inactive modules are skipped; silent voices are released; effect slots idle after their tails.
- Offline rendering uses the same code with a fixed seed and is bit-identical between runs of the same build.

### 5.5 Parameters

- One registry is the single source of truth: string id (`osc1.position`, `filter2.cutoff`, `lfo3.rate`), range, default, unit, skew, smoothing class, automatable flag, description. Host parameters, the patch schema, the interface bindings, the agent schema and `docs/PARAMETERS.md` are all generated from it.
- Host parameter ids are stable hashes of the string ids, checked for collisions in tests. The registry is append-only: parameters are never renumbered; retired ones remain as hidden placeholders.
- Effect slots expose a fixed set of generic automatable parameters whose names update with the loaded effect type; in the patch JSON they appear under their real names.
- Non-automatable state (tables, shapes, routes, clips, text) lives only in the patch document.

### 5.6 State and files

| Thing | Format |
|---|---|
| Plug-in state in a host project | Header + compressed patch JSON |
| Preset `.zpreset` | The patch JSON as readable text; binary assets embedded as base64 of deflated 32-bit float data |
| Wavetable | WAV (2048 samples per frame) with a Zederia metadata chunk |
| Shape `.zshape`, rack and module presets | JSON |
| Clip | Standard MIDI file plus hints in the preset |
| Pack `.zpack` | Zip with a manifest |

Patch documents carry a `version`; loaders migrate forward step by step; save → load → save must be byte-identical.

Locations on Windows:

| Content | Path |
|---|---|
| User content | `Documents\Zederia\` (Presets, Wavetables, Shapes, Textures, Clips, Racks, Renders, Packs) |
| Factory content | `%PROGRAMDATA%\Zederia\Factory\` (per-user fallback when not writable) |
| Settings | `%APPDATA%\Zederia\settings.json` |
| Cache, logs, link discovery | `%LOCALAPPDATA%\Zederia\` |
| Tools | `%LOCALAPPDATA%\Programs\Zederia\` |
| Plug-ins (development installs, no admin rights) | `%LOCALAPPDATA%\Programs\Common\VST3\` and `%LOCALAPPDATA%\Programs\Common\CLAP\` |

### 5.7 DSP requirements

Outcomes are binding; mechanisms are suggestions.

- **Oscillator band-limiting.** Suggested: per-table mip pyramid built by FFT, at least two levels per octave or 2× internal rate with a half-band decimator, blending across level boundaries; 4-point interpolation in phase; linear blend between neighbouring frames. Required: a full-spectrum saw swept C0–C8 at 44.1 kHz shows non-harmonic components at or below −80 dBc in Standard and −96 dBc in High.
- **Tuning.** Rendered pitch within ±0.1 cent from C0 to C8.
- **Frame morphing.** A two-second position sweep produces no discontinuity detectable by the click test (6.1).
- **Warp and cross-modulation.** Run oversampled according to quality; phase curves are pure functions of phase and amount so displays and audio agree.
- **Filters.** Topology-preserving (zero-delay feedback) forms; linear types within ±0.5 dB of their analytic response; all types stay finite under ten seconds of audio-rate cutoff modulation at maximum resonance.
- **Oversampling.** Low-latency filters so the plug-in reports zero added latency in Draft and Standard; any latency in higher modes is reported to the host.
- **Reverb.** Feedback-delay-network design with modulated lines and input diffusion; no metallic ringing on a click test; mono-compatible.
- **Parameter smoothing.** No zipper noise on any continuous control moved over its full range in 50 ms.

### 5.8 Performance budgets

Measured as realtime factor on one core at 48 kHz, 512-sample blocks, Standard quality, on the development machine recorded in `docs/ENVIRONMENT.md`. Budgets may be recalibrated once, during milestone 2, if that machine is unusually slow; after that a regression of more than 10% fails the benchmark step.

| Scenario | Budget |
|---|---|
| Idle instance (no voices, tails finished) | ≤ 0.2% |
| One voice, one oscillator | ≤ 0.5% |
| 8-voice chord, one oscillator at unison 7, one filter | ≤ 4% |
| 8 voices × 3 oscillators × unison 16, two filters, six effects | ≤ 35% (≤ 20% in Draft) |
| Interface open and animating | ≤ 2% extra |
| Workshop interaction (drawing, harmonic edits) | Feedback within 16 ms; audio update within 50 ms |
| Preset load | ≤ 100 ms without large samples |

### 5.9 Errors and logging

Recoverable problems surface in the interface and in tool results with a clear message and a suggested fix. Logs go to `%LOCALAPPDATA%\Zederia\logs\`, never from the audio thread. A corrupt preset or table never crashes a load.

---

## 6. Quality plan

### 6.1 Automated tests

- **Registry.** Unique ids and hashes; every parameter documented.
- **Patch.** Round-trip identity; migration; ten thousand malformed or fuzzed documents load without a crash.
- **Oscillators.** Tuning; aliasing thresholds; unison level compensation; click test on position sweeps (largest sample-to-sample jump compared with the expected bound for the signal).
- **Wavetable toolkit.** Each process, morph and sort operation against known inputs; formula parser (precedence, functions, error messages, complexity cap); import on synthetic signals with known pitch.
- **Filters.** Analytic response; stability sweeps; self-oscillation pitch.
- **Modulation.** Envelope segment times within ±1 ms or 1%; LFO sync phase at bar lines; matrix arithmetic including Via, curves and polarity; MPE routing.
- **Effects.** Level and tail expectations; idle detection; no output above the limiter ceiling.
- **Play.** Arpeggiator order and timing; clip playback against the source MIDI file; MIDI out content.
- **Determinism.** Same patch, notes and seed → identical output hash.
- **Real-time safety.** The audio-thread guard reports nothing across the whole test corpus.
- **Protocol.** Scripted sessions per revision and transport; every tool's inputs and outputs validated against its schemas; error paths; path sandbox; token and origin checks; cancellation.
- **Interface.** Offscreen snapshots of every page at 100%, 150% and 200%; automatic checks for clipped or overlapping components and truncated text; the builder also inspects the images.
- **Benchmarks.** The scenarios in 5.8 against stored baselines.

### 6.2 Validation and manual checks

- pluginval at strictness 10 on the VST3; clap-validator on the CLAP build.
- The standalone and at least two hosts installed on the machine: load, play, automate, save and reopen a project, render offline, open several instances.
- Agent bridge exercised from at least one real client end to end (design, render, save, clip render).

### 6.3 Reporting

Each milestone ends by updating `docs/STATUS.md` with what was built, measured results, decisions and known issues. Results are reported as observed, including failures.

---

## 7. Milestones

Each milestone corresponds to one prompt in `BUILD.md`.

| # | Milestone | Delivers | Accepted when |
|---|---|---|---|
| 0 | Environment | Toolchain verified, versions pinned, repository initialized | The environment check script passes and a smoke plug-in builds and validates |
| 1 | Foundation | Build system, parameter registry, patch model, engine skeleton, plug-in shell, CLI renderer, test and benchmark harness | Builds from scratch; tests pass; validator passes; deterministic render |
| 2 | Sources | Wavetable and classic engines, warps, cross-modulation, unison, sub, texture, wavetable toolkit, starter tables | Tuning, aliasing, click and level tests pass; budgets met |
| 3 | Shaping and motion | Two filters and routing, envelopes, LFOs, Flux, macros, matrix, voicing, tuning, MPE, character | Filter, modulation and stability tests pass; budgets met |
| 4 | Effects and play | Three-lane rack and all effects, arpeggiator, chords, clip player, MIDI out, quality modes, optimization pass | Effect and timing tests pass; all budgets in 5.8 met |
| 5 | Interface | All pages, modulation interaction, workshop, library, undo, themes, scaling, accessibility | Snapshot checks pass; every parameter reachable; interface budget met |
| 6 | Agent bridge | Protocol server, live link, tools, analysis, music tools, AGENT page, agent guide | Conformance tests pass; a real client completes the example sessions |
| 7 | Content and release | Factory tables, textures, presets and clips; hardening; installer; manuals | Full test, validation and host matrix pass; installer installs and removes without leftovers |

Factory content targets for milestone 7: about 60 wavetables and 20 textures generated from recipes, about 120 presets spread evenly across basses, leads, plucks, keys, pads, strings and brass-like sounds, sequences, effects and percussion, 24 LFO shapes, 16 clips. The presets are designed through the agent tools, so the bank doubles as proof of the bridge.

---

## 8. Risks and mitigations

| Risk | Mitigation |
|---|---|
| CPU budgets missed late | Benchmarks from milestone 1; budgets checked at the end of every engine milestone |
| Filter instability under fast modulation | Stability sweeps in tests; zero-delay-feedback forms; clamps on internal state |
| Host-specific bugs | Strict validator runs every milestone; host matrix in milestone 7; conservative use of host features |
| Interface scope | Component kit first; pages assembled from shared parts; snapshot tests instead of manual checking |
| Protocol revisions keep moving | Protocol confined to `zederia-mcp`; both generations supported; conformance tests per revision |
| Agents make destructive edits | Transactions, undo labels, revert-session, permissions, sandbox |
| Parameter list changes break saved projects | Append-only registry, stable hashed ids, migration tests |
| Framework licence | Recorded at setup; owner decides before distribution |

---

## 9. Originality and licensing

- All DSP, interface artwork, wavetables, textures, presets, clips and documentation are original work made for Zederia.
- Third-party code enters only through the dependencies in 5.1, under their licences, listed in a `NOTICE` file.
- No third-party samples, wavetables, presets, fonts or images are bundled unless their licence allows redistribution and the file is listed in `NOTICE`.
- Factory tables and textures are generated from recipes kept in `content/`, so their origin is reproducible.
- Format and protocol names are used descriptively; trademark rules for format logos are checked before any logo appears in the product.

---

## 10. After version 1

macOS port (AU, universal) · granular, spectral and multisample engines · band and mid/side splitters, convolution reverb, frequency shifter · multi-core voice rendering · tuning-server client · effect-only build with audio input into the Texture slot · pack manager.

---

## Appendix A — Parameter id conventions

- Lower-case, dot-separated: `module.parameter` or `module.group.parameter` (`osc2.unison.detune`, `fx.a3.mix`, `mod.12.amount`).
- Modules: `osc1–3`, `sub`, `texture`, `filter1–2`, `voice`, `env1–4`, `lfo1–8`, `flux1–2`, `macro1–8`, `mod.1–64`, `fx.a1–a8`, `fx.b1–b8`, `fx.m1–m8`, `arp`, `chord`, `clip`, `global`.
- Values in the patch JSON use real units (`cutoff_hz`, `attack_ms`, `level_db`); normalized 0–1 values are used only where no natural unit exists.

## Appendix B — Formula language

A formula request has a `domain` (`wave` or `harmonics`), a `scope` (current frame, selected frames, all frames) and one expression (`harmonics` may add a second expression for phase).

| Symbol | Meaning |
|---|---|
| `t` | Position in the cycle, 0…1 |
| `x` | Position in the cycle, −1…1 |
| `y` | Frame position in the table, 0…1 |
| `z` | Frame position in the table, −1…1 |
| `h` | Harmonic number (harmonics domain), 1…1024 |
| `in` | Existing value of the frame being written |
| `sel` | Existing value of the selected frame |
| `rand` | Seeded noise −1…1, fixed for each position |
| `pi`, `tau`, `e` | Constants |

Operators: `+ − * / ^ %`, comparisons, `&& || !`, and `cond ? a : b`.

Functions: `sin cos tan asin acos atan sinh cosh tanh exp ln log2 log10 sqrt abs sign floor ceil round frac min max clamp mix pow mod step smoothstep saw square tri noise`.

Expressions compile to a small bytecode with a node limit; there are no loops, variables or side effects. Errors name the position and the problem.

Examples: `sin(tau*t) * (1 - y) + saw(t) * y` (sine morphing to saw across the table); harmonics domain `1/h * (h % 2)` (odd series).

## Appendix C — Analysis descriptors

| Group | Fields |
|---|---|
| Level | Peak dBFS, RMS dBFS, integrated loudness, crest factor, DC offset, clipped samples |
| Time | Duration, attack time (10–90%), decay shape, tail to −60 dB after note-off, transient count |
| Pitch | Estimated fundamental, deviation in cents, stability, inharmonicity |
| Spectrum | Centroid, 85% roll-off, flatness (noisiness), harmonic-to-noise ratio, energy in six bands (sub, bass, low-mid, high-mid, presence, air) |
| Motion | Centroid variation over time, dominant modulation rate |
| Stereo | Width (side-to-mid ratio), correlation |
| Tags | Plain words derived from thresholds: bright, dark, plucky, sustained, wide, mono, noisy, evolving, static |
| Image | Optional mel spectrogram PNG |

## Appendix D — Glossary

**Frame** one single-cycle waveform · **Table** an ordered set of frames · **Position** where in the table the oscillator plays · **Warp** a per-oscillator transformation of phase or amplitude · **Route** one modulation connection · **Lane** one chain of effects · **Target** a headless session or live instance an agent works on · **Clip** a short note sequence stored with a preset.
