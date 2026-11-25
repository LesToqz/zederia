<p align="center"><img src="../plugin/brand/png/zederia-128.png" width="96" alt="Zederia logo: a Z cut into two slabs"></p>

# Zederia manual

Zederia is a wavetable synthesizer for Windows: a VST3 and CLAP plug-in and a Standalone
application. This manual follows the window from top to bottom and page by page. Every control
also explains itself in the help strip at the bottom of the window (switch it on in the
preferences); the last section of this manual lists all of those texts, taken from the interface
itself.

Contents:

1. [Installing](#1-installing)
2. [The window](#2-the-window)
3. [A first sound](#3-a-first-sound)
4. [The SYNTH page](#4-the-synth-page)
5. [The FX page](#5-the-fx-page)
6. [The MOD page and the modulation dock](#6-the-mod-page-and-the-modulation-dock)
7. [The TABLE page](#7-the-table-page)
8. [The PLAY page](#8-the-play-page)
9. [The LIBRARY page](#9-the-library-page)
10. [The AGENT page](#10-the-agent-page)
11. [The menu and the preferences](#11-the-menu-and-the-preferences)
12. [Files and folders](#12-files-and-folders)
13. [Keyboard shortcuts](#13-keyboard-shortcuts)
14. [Interface reference](#14-interface-reference)

## 1. Installing

Run `ZederiaSetup-<version>.exe` and accept the administrator prompt once. The installer is not
code-signed, so Windows SmartScreen may warn first; [docs/RELEASE_NOTES.md](RELEASE_NOTES.md)
says what it shows and how to continue.

| Part | Goes to |
|---|---|
| VST3 plug-in | `C:\Program Files\Common Files\VST3\Zederia.vst3` |
| CLAP plug-in | `C:\Program Files\Common Files\CLAP\Zederia.clap` |
| Standalone application, agent tools, these documents | `C:\Program Files\Zederia` |
| Factory content (presets, wavetables, textures, shapes, clips, effect racks) | `%PROGRAMDATA%\Zederia\Factory` |

The plug-in folders are the standard ones, so a host such as FL Studio finds Zederia when it
scans for plug-ins, with no extra search folder. The Standalone has a Start menu entry. Choose
**Choose the parts** on the components page to leave out a part; the factory content is always
installed.

To upgrade, run the newer installer: it removes the files of the version before it first, so
nothing stale is left. To uninstall, use **Apps & features** (Installed apps) in the Windows
settings. Both keep your own files: `Documents\Zederia` (your presets, wavetables, clips, renders
and packs) and the settings in `%APPDATA%\Zederia`.

## 2. The window

The window has four layers that stay in place on every page:

- **The header**: the logo plate, the preset bar (menu, preset name, Save, History, the A and B
  sides of the compare, Quality, FX off, the output scope, meter and master volume) and the
  **page tabs**: SYNTH, FX, MOD, TABLE, PLAY, LIBRARY and AGENT.
- **The page** you chose.
- **The modulation dock**: the eight macros, and the envelopes, LFOs and Flux generators, each
  with a chip you drag onto a control to modulate it, its graph and its controls.
- **The keyboard**: the pitch and modulation wheels, octave and velocity for the computer keys,
  and the keys themselves.

Things that work everywhere:

- **Help.** The help strip at the bottom names what is under the pointer and says what it does;
  F1 keeps it on one control. It is hidden until you switch it on in the preferences. Tooltips
  say the same beside the pointer.
- **Values.** Drag a knob up and down (or around it, in the preferences); hold Shift for fine
  steps; double-click to reset it (or to type, in the preferences). Right-click any control to
  type a value with its unit (`2k`, `-6dB`, `1/8.`, `A#3`), reset it, MIDI-learn it, lock it so
  presets leave it alone, copy or paste it, and add or see its modulation.
- **Undo.** Every change is a named step. Ctrl+Z undoes and Ctrl+Shift+Z (or Ctrl+Y) redoes;
  History lists the steps, and a click on one goes back to it.
- **Compare.** A and B hold two versions of the sound. Click the other letter (or Ctrl+B) to
  switch; Copy to other side makes both the same.
- **Find.** Ctrl+K opens the command palette: type any control, action, page or preset.
- **Simple view** hides the advanced controls. Everything stays reachable from Full view and the
  palette.
- **Size.** Drag the window's corner, from 70 % to 300 % of 1320 x 940; the preferences set the
  size new windows open at, and the text size separately.
- **Computer keys.** The letters play notes in two rows, `Z S X D C V G B H N J M , L . ; /`
  from C and `Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P [ = ]` from the C an octave higher. Left and
  Right move the octave, Down and Up the velocity. Letters typed into a text field stay there.

## 3. A first sound

1. Open the LIBRARY page and click a preset: one click loads it. The arrow keys move through the
   list, loading as they go.
2. Play it with a MIDI keyboard, the computer keys or the keys on screen.
3. Turn the macros in the dock: each preset names its macros for what they change.
4. On the SYNTH page, change the table position of an oscillator, the filter cutoff, the amp
   envelope in the dock (Env 1).
5. Save with Save (Ctrl+S): give a name, a folder inside `Documents\Zederia\Presets`, a type and
   tags. To start from nothing, choose **Init** in the menu: a new instance starts from the
   factory's Default preset.

## 4. The SYNTH page

The sound sources and the voice.

- **Osc 1, Osc 2, Osc 3** side by side. Each has a power switch (an oscillator that is off costs
  nothing), an engine (Wavetable, or Classic: a low-cost saw, square, triangle and sine), its
  table display (3D shows the frames in depth with the playing position lit, 2D the cycle that
  plays now), the table's name (click to choose another, load a file or open it on the TABLE
  page), Position, Level, Pan and Fine on the first row, and four tabs for the rest:
  - **Unison**: up to 16 stacked copies, with detune, blend, width and range.
  - **Spread**: how the unison voices differ: the detune law, stacked octaves or fifths, their
    start phases, and spreads of table position and warp amount across them.
  - **Warp**: two warp slots that bend the cycle (sync, bend, mirror, quantize, fold, drawn curves
    and more) with an amount each; the pm, am and rm modes take another source (an oscillator,
    the Sub or the Texture) as the modulator.
  - **Pitch**: octave and semitone offsets, key tracking (off gives a fixed pitch for drones and
    modulators), the start phase and its randomness, and the oscillator's share to Filter 1 or 2
    and to Direct.
- **Sub**: a simple oscillator below the note (also usable as a modulator), with its octave and
  shape. **Texture**: a stereo sample player for noise beds, breath, hiss and one-shot
  transients, with start, random start, loop or one-shot, and key tracking. Each source has its
  level, pan and send to Filter 1 or Filter 2, or Direct to skip the filters.
- **The Stage**, beside the sources, shows what you are working on: the filter response (the faint
  curve for the settings, the bright one for the modulated state; click for phase), a warp curve,
  or the Sub's or Texture's output. Its **Signal map** button opens the routing of the whole
  sound; click a part of it to go there.
- **Filter 1 and Filter 2**: 70 types in 16 families (state-variable, ladder, dual, morph, comb,
  flange, phase, formant, shelf and bell, ring modulator, sample and hold, all-pass cluster,
  diffuser, band-reject, howl, grit comb). Cutoff, resonance, drive, a Character control whose
  meaning depends on the type, stereo offset, key tracking, and Mix (or Level).
- **Voice**, in seven tabs: ROUTE (the filters in series or parallel, and each bus's send to
  effect lane A or B), VOICE (poly, mono or legato, polyphony, voice stealing, note priority,
  velocity sensitivity), GLIDE, CHAR (drift, warmth, a noise floor, and per-note variance of
  cutoff, envelope times, pan and level), TUNE (master tune, reference pitch, bend ranges, and
  Scala `.scl` and `.kbm` or `.tun` tunings with a lock that keeps the tuning when presets load),
  MIDI (MPE, MIDI out and thru, the envelope follower's source and times) and OUT (output
  protection, the DC filter, and the playing and offline quality).

## 5. The FX page

Three lanes of up to eight effects: **Lane A**, **Lane B** (beside lane A, or after it) and
**Master**. The voice buses go to lanes A and B as the Voice sends say, and both lanes go into
Master.

- **The chain** on the left: the lane tabs, the lane's effects in order, and Add effect. Drag an
  effect to reorder it, or onto another lane's tab to move it there. Any effect can be used in
  any slot, any number of times.
- **The rack card**: the bus sends, FX off, **Lock rack** (keeps the rack when presets load),
  and rack presets (save, load from the factory or your Racks folder, copy and paste).
- **The strips** on the right: every effect of the lane with its controls in named groups, its
  power switch and mix, and a display where one helps (EQ and filter responses, Drive's curves,
  the Widener's correlation). Controls that do nothing in the current setting are hidden, such as
  a rate while Sync is on. Each strip can save and load single-effect presets.

The effects: Drive (fifteen modes, two of them drawn curves), Compressor (one band or three),
EQ (four bands), Filter (any voice filter type), Chorus, Flanger, Phaser, Delay (dual, ping-pong,
pre-tap; times or tempo divisions), Reverb (plate, hall, room), Swarm, Widener, Utility and
Limiter. Every change to the rack, even while notes play, happens without clicks.

## 6. The MOD page and the modulation dock

**The dock** (below every page) holds the modulators:

- **Macros 1 to 8**: drag a macro's chip onto any control to let it move that control;
  double-click a macro to rename it. A macro is 0 to 1, or -1 to 1 around the middle.
- **Env 1 to Env 4**: delay, attack, hold, decay, sustain and release, with a curve on attack,
  decay and release; drag the points in the graph. Env 1 is the amp envelope. Any time can be a
  note length (type `1/8`, `1/4T`, `3/16`) that follows the tempo. Envelopes 2 to 4 also have a
  multi-segment mode with a loop.
- **LFO 1 to LFO 8**: drawn shapes (start from Shapes: the basic ones, the factory's or your own),
  free or synced rates up to audio rate, retrigger, one-shot or loop modes, delay, rise, phase,
  smoothing, and per voice or shared.
- **Flux 1 and 2**: random generators, free or synced, shared or per voice.

To modulate, drag a chip onto a control. The control then shows an arc for the depth and the
live value moving. Click a chip to select it, then Alt-drag on a control to set the depth;
Shift+Alt-click flips the polarity. Hover a modulated control to see its routes with their
amounts, and to mute, solo or remove one. **Mod map** (Ctrl+M) draws every connection on the page.

**The MOD page** lists all routes (up to 64): source, Via (a second source that scales the
route, invertible), destination, amount, polarity and curve (linear, exponential, logarithmic,
S or drawn). Add route asks for a source and then a destination. Search, sort, and check routes
to replace their source, mute them or remove them together. Sources also include velocity,
release velocity, note, pitch bend, the mod wheel, aftertouch, MPE pressure and slide,
expression, breath, the sustain pedal, any controller (`cc74`), random values, voice and unison
index, the audio of the sources, an envelope follower and the bar position.

## 7. The TABLE page

The wavetable workshop for the oscillator chosen at the top (Osc 1, 2 or 3). A table is up to
256 key frames of one cycle each; Morph makes the positions between them.

- **Frames**: the key frames as a strip. Click to select, Shift for a range, Ctrl for several;
  drag to reorder. Insert, Duplicate, Delete, Thin, Crop and Reverse work on the selection.
- **Wave**: draw on the selected frame with the drawing shapes (Free, Line, Flat, ramps, Sine,
  Curve and more), with Snap to a grid. Faint lines are the neighbouring frames.
- **Harmonics**: the level (top) and phase (bottom) of each harmonic of the frame; drag across
  the bars to draw them, and right-click for operations.
- **Import** makes a table from audio (single cycles, recordings, spectral snapshots) or an
  image, with a preview before it is kept. **Export** writes the table, the selection or each
  frame as a single-cycle file. **Process** (normalize, fades, filters, splits and more),
  **Morph**, **Sort** (by brightness, partials or fundamental) and **Resynth** (bake a warp or the
  whole sound into a table, or an LFO shape into a frame) work on the selected frames or all.
- **Save table** keeps it in `Documents\Zederia\Wavetables`, where the oscillator then finds it by
  name. A table you edit and do not save stays inside the preset.

## 8. The PLAY page

What turns the keys you play into other notes.

- **Arpeggiator**: modes (up, down, up-down, random, walk, as played, chord), one to four
  octaves, a synced rate, gate, swing, latch and restart, and a step lane of up to 32 steps where
  each step is on, tied or a rest, with its velocity, transposition and ratchets. Twelve pattern
  slots; copy and paste patterns between them.
- **Chords and scale**: chord memory (one key plays a stored chord; click the semitones or learn
  the chord from held keys), scale lock (fourteen scales; snap to the nearest note or drop the
  others) and strum (up, down, alternate).
- **Clip player**: eight clip slots with a piano roll. Trigger by the host's transport, or by a
  key (which transposes the clip). Latch and loop. Import a clip from the factory, your Clips
  folder or any MIDI file (or drop one on the page), export it as a MIDI file, or drag it from
  the page into your host's arrangement.
- **MIDI out** sends the notes these features make to the host; **MIDI thru** passes incoming
  MIDI on unchanged.

## 9. The LIBRARY page

Presets from the factory (`Factory`) and your own (`Yours`).

- **One click loads a preset**; the arrow keys move and load. Ctrl and Shift build a selection
  for bulk edits and load nothing.
- **The left column**: PRESETS (the Presets folder as a tree, with counts), then sections to
  filter by SHOW, TYPE, CHARACTER, GENRE, PACK and AUTHOR. Each folds away from its heading.
- **Search** takes words and `field:value` filters such as `type:bass tag:warm genre:house
  author:me rating:3 fav`.
- **With presets selected**: ratings, tags, favourites, moving, pack export and Blend (a new
  sound between two presets).
- **Import pack** reads a `.zpack` file or a folder of presets into `User Presets/<pack name>`;
  sounds already in the library are skipped. **Find duplicates** selects presets that sound the
  same. **Module presets** save or load a single module (an oscillator, a filter, an envelope, an
  LFO, an effect, an arpeggiator pattern or a clip).

## 10. The AGENT page

AI agents can design sounds with Zederia through `zederia-mcp`, the agent bridge installed with
the agent tools. They work in sessions of their own, or on this instance while you listen.

- **Agent link** switches this instance's local link on or off (off by default; the header shows
  when it is on). Only programs running as you on this computer can reach it.
- **Activity** lists what agents did, here and in their own sessions. **Revert agent session**
  brings back the sound from before the first agent edit, as one undo step. Every agent change is
  also a named step in History.
- **Permissions for every agent on this computer**: reading and editing the patch, writing in
  Zederia's folders, writing in folders you allow, auditioning through the speakers below a level
  cap, and reading samples in folders you allow.
- **Client setup** copies the configuration (or a setup prompt) for Claude Code, Codex or
  Antigravity, with the path of `zederia-mcp.exe` on this computer.

`docs/AGENT_GUIDE.md` is the agents' own guide; the server also gives it to them.

## 11. The menu and the preferences

The menu at the left of the preset bar:

- **Init**, **Open preset file...**, **Save preset as...**
- **Variation**: Randomize (by scope: sources only, effects only, or everything), Mutate (small
  changes), and Blend presets (in the library).
- **Locks**: keep the effect rack, the tuning, or polyphony and voice mode when presets load, or
  unlock every parameter.
- **MIDI**: save, load and clear controller maps (from MIDI learn), and a program list from a
  folder (program changes then load its presets).
- **Preferences...**, **Command palette**, **History**.

The preferences: Default size and Text size (90 to 150 %), Knob drag (vertical or around),
Mouse wheel (scroll the page, change values, or fine changes), Double-click (reset or type),
Tooltips, Help strip, Computer keys and Key letters, View (Full or Simple), Frame rate (15, 30
or 60 per second), Rendering (software, or the graphics card), MPE for new instances, Default
preset, Offline quality, the default author for saved presets, Content folder, and Agent link.

## 12. Files and folders

| What | Where |
|---|---|
| Your presets | `Documents\Zederia\Presets` (agents save in `Presets\Agent`) |
| Your wavetables, textures, shapes, clips, effect racks | `Documents\Zederia\Wavetables`, `Textures`, `Shapes`, `Clips`, `Racks` |
| Renders and packs from agents | `Documents\Zederia\Renders`, `Documents\Zederia\Packs` |
| Factory content (read-only) | `%PROGRAMDATA%\Zederia\Factory` |
| Settings, agent permissions | `%APPDATA%\Zederia` |
| Agent link discovery files | `%LOCALAPPDATA%\Zederia\link` |

The Content folder preference moves `Documents\Zederia` elsewhere. Presets are JSON documents
(`.zpreset`); a preset that uses a table you edited carries the table inside it, so it plays the
same on another computer.

## 13. Keyboard shortcuts

| Keys | Does |
|---|---|
| Ctrl+Z, Ctrl+Shift+Z or Ctrl+Y | Undo, redo |
| Ctrl+S | Save the preset |
| Ctrl+K | Command palette |
| Ctrl+B | Switch between A and B |
| Ctrl+M | Mod map |
| F1 | Keep the help strip on the control under the pointer |
| Left, Right | Computer keys an octave down, up |
| Down, Up | Computer keys softer, harder |
| Tab | Move the keyboard focus between controls |
| Arrow keys, Space, Delete | Edit a focused part that has keys of its own (step lane, piano roll, frames, curves) |

## 14. Interface reference

What the help strip says for every control, page by page and panel by panel. Panels that repeat
(Osc 2 and Osc 3 after Osc 1) are named beside the first one.

<!-- ui-help: begin (generated by zederia-cli ui-help from the interface's help-strip texts; do not edit by hand) -->

### Around every page

#### On the page

| Control | Help strip |
|---|---|
| Voice allocation | Sounding voices, including releases, and the current voice limit. |
| Menu | Init, open and save presets, MIDI learn and controller maps, programs, preferences, theme and size. |
| Preset | The sound's name; click to open the library. A dot marks changes since it was loaded or saved. |
| Save | Save the sound as a preset (Ctrl+S). |
| History | The list of changes; click one to go back to it. Ctrl+Z undoes, Ctrl+Shift+Z redoes. |
| Compare A, Compare B | Two versions of the sound to compare. Click the other letter to switch (Ctrl+B). |
| Copy to other side | Copy this side to the other side of the compare. |
| Quality | Oversampling while playing: Draft costs least, Ultra the most. Offline renders use the offline quality (preferences). |
| Global FX Off | Bypasses the whole effects rack. |
| Output scope | See the mono output waveform and spectrum. Capture a reference to compare the next edit against it. |
| Output | Peak level of the output, left and right; the mark is 0 dBFS. |
| Global Master Volume | Output level after the effects. |
| Simple view | Hides the advanced controls; everything stays reachable from Full view and the command palette. |
| Mod map | Draws every modulation connection on this page at once (Ctrl+M). |
| Find | The command palette: any control, action or preset by name (Ctrl+K). |
| Preferences | Theme, size, knob and wheel behaviour, frame rate, rendering, default author and content folder. |
| Pitch | Bends the pitch; springs back when released. |
| Mod | The modulation wheel (controller 1). |
| Octave down | Moves the computer keys an octave down (Left arrow). |
| Octave up | Moves the computer keys an octave up (Right arrow). |
| Keyboard | Click or drag to play. The computer keyboard plays too (Computer keys in the preferences): Z S X D C V G B H N J M , L . ; / play from C of the octave, Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P [ = ] from C an octave higher; Left and Right move the octave, Down and Up the velocity. |
| Velocity down | Softer computer keys (Down arrow). |
| Velocity up | Harder computer keys (Up arrow). |

#### Macros

| Control | Help strip |
|---|---|
| Macro 1 to 8 | Drag onto any control to let this macro move it. Double-click to rename. |
| Macro 1 to 8 Value | Position of the macro. |
| Macro range | 0 to 1 (+), or -1 to 1 around the middle. |

#### Env 1

| Control | Help strip |
|---|---|
| Env 1 to 4, LFO 1 to 8, Flux 1 to 2 | Drag onto any control to modulate it. Click to select it: Alt-drag on a control then sets its depth. |
| Envelope | Drag the points: across for the times, up and down at the decay point for the sustain level. Hold Shift for fine moves. |
| Env 1 Attack | Time to rise to full level. Type a note length (1/8, 1/4T, 3/16) to follow the tempo. |
| Env 1 Decay | Time to fall to the sustain level. Type a note length (1/8, 1/4T, 3/16) to follow the tempo. |
| Env 1 Sustain | Level held while the key is down. |
| Env 1 Release | Time to fall to silence after the key is released. Type a note length (1/8, 1/4T, 3/16) to follow the tempo. |
| Env 1 Attack Curve | Shape of the attack: 0 is linear, positive moves fast at first then eases, negative the reverse. |
| Env 1 Delay | Wait before the attack starts. Type a note length (1/8, 1/4T, 3/16) to follow the tempo. |
| Env 1 Hold | Time held at full level before the decay. Type a note length (1/8, 1/4T, 3/16) to follow the tempo. |
| Env 1 Decay Curve | Shape of the decay: 0 is linear, positive moves fast at first then eases, negative the reverse. |
| Env 1 Release Curve | Shape of the release: 0 is linear, positive moves fast at first then eases, negative the reverse. |

#### Env 2

| Control | Help strip |
|---|---|
| Env 2 Mode | Standard is delay-attack-hold-decay-sustain-release; Multi is a drawn multi-segment envelope with a sustain point and loop region. |

#### LFO 1

| Control | Help strip |
|---|---|
| Shapes | Start from a basic shape, or a shape from the factory or your Shapes folder. |
| Step drawing | Drag across the grid to draw steps. |
| Snap | Points snap to the grid (hold Alt while dragging to move freely). |
| Copy shape | Copy this shape; paste it into any LFO, envelope, warp curve or drive curve. |
| Paste shape | Paste a copied shape here. |
| Env 1 to 4, LFO 1 to 8, Flux 1 to 2 | Drag onto any control to modulate it. Click to select it: Alt-drag on a control then sets its depth. |
| LFO shape | Double-click to add or remove a point. Drag points; drag the small diamonds to bend a segment. Shift-click or drag a frame to select several; Ctrl-drag scales them. Right-click for grid, snapping, step drawing, shapes, copy and paste. |
| LFO 1 Rate | Free rate; values above 100 Hz take effect with audio-rate mode on. |
| LFO 1 Phase | Start phase. |
| LFO 1 Delay | Wait after note-on before the LFO starts. |
| LFO 1 Rise | Fade-in time of the LFO depth. |
| LFO 1 Smooth | Rounds off steps and corners in the shape. |
| LFO 1 Rate Key Tracking | How far the rate follows the keyboard. |
| LFO 1 Mode | Free ignores notes, Retrigger restarts on each note, One-shot plays once like an envelope, Loop-point plays through then loops from the shape's loop marker. |
| LFO 1 Sync | Locks the rate to the host tempo using the division. |
| LFO 1 Anchor | Keeps a synced LFO locked to song position when its rate changes. |
| LFO 1 Polarity | Output range: -1..1 or 0..1. |
| LFO 1 Scope | Per-voice LFOs run separately for every note; a global LFO is shared by all. |
| LFO 1 Audio Rate | Evaluates the LFO every sample so it can run up to 2 kHz. |

#### Flux 1

| Control | Help strip |
|---|---|
| Env 1 to 4, LFO 1 to 8, Flux 1 to 2 | Drag onto any control to modulate it. Click to select it: Alt-drag on a control then sets its depth. |
| Output | The modulator's output over the last seconds. |
| Flux 1 Rate | Free rate. |
| Flux 1 Type | Orbit is smooth and loosely periodic, Burst restarts irregularly, Stepped is sample-and-hold, Smooth is filtered noise. |
| Flux 1 Sync | Locks the rate to the host tempo using the division. |
| Flux 1 Scope | One generator per voice, or one shared by all. |

### The SYNTH page

#### Osc 1 (also Osc 2, Osc 3)

| Control | Help strip |
|---|---|
| Osc 1 On | Power switch; an oscillator that is off costs nothing. |
| Osc 1 Engine | Wavetable plays the loaded table; Classic is a low-cost virtual-analog saw, square, triangle and sine. |
| Table | The table in depth with the playing position lit, or the cycle as it sounds now. The 3D / 2D button switches. |
| Table view | 3D shows the frames of the table in depth with the playing position lit; 2D the cycle that plays now. |
| Table | The wavetable this oscillator plays; click to choose another, load a file or open it in the workshop. |
| Osc 1 Position | Where in the wavetable the oscillator plays, continuous across frames. |
| Osc 1 Level | Output level of the oscillator. |
| Osc 1 Pan | Stereo position. |
| Osc 1 Fine | Fine pitch offset. |
| Osc 1 Coarse | Continuous pitch offset, intended as a modulation destination. |
| Osc 1 Unison Detune | Detune spread as a share of the unison range. |
| Osc 1 Unison Blend | Level of the side voices against the centre voice. |
| Osc 1 Unison Width | Stereo spread of the unison voices. |
| Osc 1 Unison Range | Largest detune spread. |
| Osc 1 Unison Voices | Number of stacked detuned copies. |
| Osc 1 Unison Spread Law | How detune is distributed: evenly, denser near the centre, exponential, inverse, or random per note. |
| Osc 1 Unison Stack | Transposes alternate voices: +12, +12/+24, fifths, fifths and octaves, or the centre voice down. |
| Osc 1 Unison Start Phase | Free keeps running, Random differs per note, Consistent reuses one stored set of random phases. |
| Osc 1 Unison Position Spread | Spreads wavetable position across the voices. |
| Osc 1 Unison Warp Spread | Spreads the warp amount across the voices. |
| Osc 1 Warp A Mode, Osc 1 Warp B Mode | What this warp slot does to the cycle; pm, am and rm take another source as the modulator. The Classic engine uses fold, drive, pm, am and rm only: the phase-shaping modes need a wavetable. |
| Osc 1 Warp A Amount, Osc 1 Warp B Amount | Strength of the warp; two-sided modes (bend_both, skew_both) are neutral at 50 %. |
| Osc 1 Warp A Source, Osc 1 Warp B Source | The source used as modulator by the pm, am and rm modes; it may be silent in the mix. |
| Edit curve | Draw this warp's phase curve in the display below (the curve modes use it). |
| Osc 1 Octave | Pitch offset in octaves. |
| Osc 1 Semitones | Pitch offset in semitones. |
| Osc 1 Key Tracking | On follows the keyboard; off plays a fixed pitch for drones, percussion and modulator duty. |
| Osc 1 Phase Mode | Reset starts every note at the start phase; Continue carries on from the last note. |
| Osc 1 Phase | Start phase of each note. |
| Osc 1 Phase Random | How much the start phase is randomized per note. |
| Osc 1 Filter Balance | Share sent to Filter 1 (0 %) or Filter 2 (100 %). |
| Osc 1 Direct | Share that skips both filters. |

#### Filter 1 (also Filter 2)

| Control | Help strip |
|---|---|
| Filter 1 On | Power switch; a filter that is off passes the signal unchanged. |
| Filter 1 Type | Filter family and response. |
| Filter 1 Cutoff | Cutoff or centre frequency. |
| Filter 1 Resonance | Emphasis around the cutoff; some types self-oscillate at the top. |
| Filter 1 Drive | Input gain into the filter's nonlinear stages. |
| Filter 1 Character | Type-dependent control (body, heat, second cutoff, morph position, damping, formant shift, gain); two-sided controls are neutral at 50 %. |
| Filter 1 Stereo Offset | Moves the left and right cutoffs apart. |
| Filter 1 Key Tracking | How far the cutoff follows the keyboard. |
| Filter 1 Mix | Dry/wet mix, or output level when Mix Mode is Level. |
| Filter 1 Mix Mode | Mix blends dry and filtered signal; Level trims the filtered output with no dry signal. |

#### Filter response

| Control | Help strip |
|---|---|
| Signal map | The routing of the whole sound; click a part to go to it. |
| Filter response | The faint curve follows the patch settings and the bright curve follows modulation. Click to switch between magnitude and phase. |

#### Sub

| Control | Help strip |
|---|---|
| Sub On | Power switch. |
| Sub Direct | Skips the filters. |
| Sub Level | Output level. |
| Sub Pan | Stereo position. |
| Sub Fine | Fine pitch offset. |
| Sub Coarse | Continuous pitch offset, so the sub can modulate at any ratio. |
| Sub Filter Balance | Share sent to Filter 1 (0 %) or Filter 2 (100 %). |
| Sub Octave | Octave below the played note. |
| Sub Shape | Waveform of the sub oscillator. |

#### Texture

| Control | Help strip |
|---|---|
| Texture On | Power switch. |
| Texture Direct | Skips the filters. |
| Sample | The sample the Texture player plays; click to choose another or load a file. |
| Texture Level | Output level. |
| Texture Pan | Stereo position. |
| Texture Pitch | Transposition. |
| Texture Fine | Fine pitch offset. |
| Texture Start | Where in the sample playback begins. |
| Texture Random Start | How much the start position is randomized per note. |
| Texture Filter Balance | Share sent to Filter 1 (0 %) or Filter 2 (100 %). |
| Texture Mode | One-shot plays the sample once; Loop repeats it, using loop points stored in the file when present. |
| Texture Key Tracking | When on, the sample is transposed with the keyboard. |

#### Voice

| Control | Help strip |
|---|---|
| Voice Filter Routing | Blend between the filters in series (0 %: Filter 1 into Filter 2) and in parallel (100 %). |
| FX Filter 1 Send | Share of Filter 1's output sent to lane A (0 %) or lane B (100 %). |
| FX Filter 2 Send | Share of Filter 2's output sent to lane A (0 %) or lane B (100 %). |
| FX Direct Send | Share of the direct signal sent to lane A (0 %) or lane B (100 %). |
| Global Voice Mode | Poly plays chords; Mono plays one note and retriggers; Legato plays one note and keeps the envelopes running while keys overlap. |
| Global Polyphony | Largest number of notes sounding at once; also the voice limit. |
| Voice Stealing | Which voice gives way when the limit is reached: the oldest, the quietest, or one that is already releasing (falling back to the oldest). |
| Voice Note Priority | Which held key sounds in Mono and Legato modes. |
| Voice Velocity Sensitivity | How strongly key velocity scales the level. |
| Global Glide Time | Time for pitch to slide from the previous note to the new one. |
| Global Glide Curve | Shape of the slide: 0 is linear in pitch, positive arrives quickly then eases, negative starts slowly. |
| Global Glide Mode | Slide on every note, or only when a new key is pressed while another is held. |
| Global Glide Scaled | When on, the glide time is per octave, so larger intervals take longer. |
| Voice Drift | Slow random pitch wander, different for every voice. |
| Voice Warmth | Gentle saturation before the filters with a soft high-frequency tilt. |
| Voice Noise Floor | A low bed of analog-style noise under each voice. |
| Voice Cutoff Variance | Random filter cutoff offset chosen per note. |
| Voice Envelope Variance | Random scaling of envelope times chosen per note. |
| Voice Pan Variance | Random pan offset chosen per note. |
| Voice Level Variance | Random level offset chosen per note. |
| Global Master Tune | Fine tuning of the whole instrument, to 0.1 cent. |
| Global Reference Pitch | Frequency of the A above middle C. |
| Global Bend Up | Pitch-bend range upward. |
| Global Bend Down | Pitch-bend range downward. |
| Load tuning | Scala (.scl, with an optional .kbm beside it) or .tun files. |
| Equal temperament | Back to twelve equal steps per octave. |
| Lock tuning | Keeps this tuning when presets load. |
| Global MPE Bend Range | Per-note pitch-bend range in MPE mode. |
| Global MPE | Treats each MIDI channel as one note with its own bend, pressure and slide. |
| Global MIDI Out | Sends notes made by the arpeggiator, chord tools and clip player to the host. |
| Global MIDI Thru | Passes incoming MIDI on to the host unchanged. |
| Global Follower Source | Signal the envelope follower listens to: the instrument before its effects, or the side-chain input. |
| Global Follower Attack | How quickly the envelope follower rises. |
| Global Follower Release | How quickly the envelope follower falls. |
| Global Output Protection | Optional soft clipper or limiter on the final output. |
| Global DC Filter | Removes any constant offset from the output. |
| Global Quality | Oversampling used while playing: Draft none, Standard 2x, High 4x, Ultra 8x on warped and nonlinear paths. |
| Global Offline Quality | Quality used when the host renders faster than real time and for offline renders. |

### The FX page

#### On the page

| Control | Help strip |
|---|---|
| Drive, Compressor, EQ, Filter, Chorus, Flanger, Phaser, Delay, Reverb, Swarm, Widener, Utility, Limiter | Click to select. Right-click (or the menu button) for presets, copy, paste, moving and removing. |
| Spectrum | The instrument's output, by frequency. |
| Mono compatibility | Correlation of left and right: +1 is mono, 0 unrelated, below 0 parts cancel when summed to mono. |
| Add effect | Puts an effect at this position of the lane; any effect, any number of times, up to eight per lane. |

#### Chain

| Control | Help strip |
|---|---|
| Lane A, Lane B, Master | Show this lane's effects. Drop an effect here to move it to the end of this lane. |
| Lane B | Parallel: lane B runs beside lane A. In series: after it. |
| Drive, Compressor, EQ, Filter, Chorus, Flanger, Phaser, Delay, Reverb, Swarm, Widener, Utility, Limiter | Click to edit. Drag to reorder or move to another lane. Right-click for presets, copy, paste, replace and remove. |
| Add effect | Any effect, any number of times; up to eight per lane. |

#### Rack

| Control | Help strip |
|---|---|
| FX Filter 1 Send | Share of Filter 1's output sent to lane A (0 %) or lane B (100 %). |
| FX Filter 2 Send | Share of Filter 2's output sent to lane A (0 %) or lane B (100 %). |
| FX Direct Send | Share of the direct signal sent to lane A (0 %) or lane B (100 %). |
| Global FX Off | Bypasses the whole effects rack. |
| Lock rack | Keeps this rack and its settings when presets load. |
| Save rack | Save all three lanes as a rack preset. |
| Load rack | Load a rack preset from the factory or your Racks folder into all three lanes. |
| Copy rack | Copy all three lanes; paste them into another instance or preset. |
| Paste rack | Replace the rack with a copied one. |

#### Drive

| Control | Help strip |
|---|---|
| On | Power switch of the slot. |
| Amount | How hard the signal is driven. |
| Level | Output trim. |
| Mix | Dry/wet mix of the slot. |
| Mode | Distortion character. |
| Filter Position | Places the filter before or after the distortion. |

#### Compressor

| Control | Help strip |
|---|---|
| On | Power switch of the slot. |
| Threshold | Level above which gain is reduced. |
| Ratio | Compression ratio; the maximum acts as a limiter. |
| Attack | Attack time. |
| Release | Release time. |
| Make-up | Gain after compression. |
| Mix | Dry/wet mix of the slot. |
| Mode | One band, or three bands. |

#### EQ

| Control | Help strip |
|---|---|
| On | Power switch of the slot. |
| Low Frequency | Frequency of the low band. |
| Low Gain | Gain of the low band. |
| Low Q | Width or slope of the low band. |
| Mid 1 to 2 Frequency | Frequency of the bell. |
| Mid 1 to 2 Gain | Gain of the bell. |
| Mid 1 to 2 Q | Width of the bell. |
| Low Type | Shape of the low band. |
| Mix | Dry/wet mix of the slot. |
| High Frequency | Frequency of the high band. |
| High Gain | Gain of the high band. |
| High Q | Width or slope of the high band. |
| High Type | Shape of the high band. |

#### Filter

| Control | Help strip |
|---|---|
| On | Power switch of the slot. |
| Cutoff | Cutoff or centre frequency. |
| Resonance | Emphasis around the cutoff. |
| Drive | Input gain into the nonlinear stages. |
| Character | Type-dependent control, as for the voice filters. |
| Stereo Offset | Moves the left and right cutoffs apart. |
| Filter Type | Filter family and response. |
| Mix | Dry/wet mix of the slot. |

#### Chorus

| Control | Help strip |
|---|---|
| On | Power switch of the slot. |
| Rate | Modulation rate. |
| Depth | Modulation depth. |
| Delay 1 | First delay time. |
| Delay 2 | Second delay time. |
| Feedback | Amount fed back into the delays. |
| Wet Filter Cutoff | Cutoff of the wet-signal filter. |
| Mix | Dry/wet mix of the slot. |
| Voices | Number of chorus voices. |
| Sync | Locks the rate to the host tempo. |
| Wet Filter | Filter on the wet signal. |

#### Flanger

| Control | Help strip |
|---|---|
| On | Power switch of the slot. |
| Rate | Modulation rate. |
| Depth | Modulation depth. |
| Feedback | Feedback amount; negative values invert it. |
| Stereo Phase | Phase offset between left and right. |
| Mix | Dry/wet mix of the slot. |
| Sync | Locks the rate to the host tempo. |

#### Phaser

| Control | Help strip |
|---|---|
| On | Power switch of the slot. |
| Rate | Modulation rate. |
| Depth | Modulation depth. |
| Centre | Centre frequency of the sweep. |
| Feedback | Feedback amount; negative values invert it. |
| Stereo Phase | Phase offset between left and right. |
| Mix | Dry/wet mix of the slot. |
| Sync | Locks the rate to the host tempo. |
| Stages | Number of all-pass stages. |

#### Delay

| Control | Help strip |
|---|---|
| On | Power switch of the slot. |
| Offset L | Fine offset of the left synced time. |
| Feedback | Amount fed back into the delay. |
| Feedback Band Centre | Centre of the band-pass in the feedback path. |
| Feedback Band Width | Width of the band-pass in the feedback path. |
| Saturation | Saturation in the feedback path. |
| Modulation | Slow modulation of the delay time. |
| Ducking | Lowers the echoes while the input is loud. |
| Mix | Dry/wet mix of the slot. |
| Mode | Delay topology. |
| Sync | Uses the divisions instead of the times in milliseconds. |
| Link | Makes the right side follow the left. |
| Division L | Left delay length when synced. |

#### Reverb

| Control | Help strip |
|---|---|
| On | Power switch of the slot. |
| Size | Size of the space. |
| Decay | Time for the tail to fall by 60 dB. |
| Pre-delay | Gap before the reverb starts. |
| Low Cut | Removes lows from the reverb. |
| High Cut | Removes highs from the reverb. |
| Damping | How quickly highs decay inside the tail. |
| Modulation | Movement of the delay lines. |
| Width | Stereo width of the tail. |
| Mix | Dry/wet mix of the slot. |
| Mode | Reverb character. |
| Freeze | Holds the current tail indefinitely. |

#### Swarm

| Control | Help strip |
|---|---|
| On | Power switch of the slot. |
| Rate | Speed of the detune movement. |
| Detune | Depth of the detune. |
| Mix | Dry/wet mix of the slot. |
| Voices | Number of detuned voices. |
| Restart | Restarts the movement on each note. |

#### Widener

| Control | Help strip |
|---|---|
| On | Power switch of the slot. |
| Size | Spacing of the taps. |
| Mix | Dry/wet mix of the slot. |

#### Utility

| Control | Help strip |
|---|---|
| On | Power switch of the slot. |
| Gain | Level change. |
| Pan | Stereo position. |
| Width | Stereo width; 0 % is mono. |
| Mono Below | Makes the signal mono below this frequency; 0 is off. |
| Mix | Dry/wet mix of the slot. |
| Polarity | Flips the polarity of one or both channels. |
| DC Filter | Removes any constant offset. |

#### Limiter

| Control | Help strip |
|---|---|
| On | Power switch of the slot. |
| Ceiling | Highest output level. |
| Release | Release time. |
| Mix | Dry/wet mix of the slot. |

### The MOD page

#### On the page

| Control | Help strip |
|---|---|
| Add route | Choose a source, then a destination. Dragging a chip onto a control does the same. |
| Sort | Order the routes by number, source, destination or amount. |
| Replace source | Give every selected route a new source. |
| Mute selected | Mute (or unmute) every selected route. |
| Remove selected | Remove every selected route. |
| Select all | Checks every route the list shows (with a search, the routes that match it); again to clear. |
| Mod map | Every connection drawn over the SYNTH page at once. |

### The TABLE page

#### On the page

| Control | Help strip |
|---|---|
| Osc 1 to 3 | Edit this oscillator's table. |
| Import | Make a table from audio or an image, with a preview before it is kept. |
| Export | Write the table, the selection, or each frame as a single-cycle file. |
| Save table | Save the table in your Wavetables folder; the oscillator then plays it by name. |
| Morph | How the positions between key frames are made, and baking them into frames. |
| Process | Normalize, fades, filters, splits and more, on the selected frames or all. |
| Sort | Reorder the frames by brightness, partials or fundamental. |
| Resynth | Bake a warp or the whole sound into a table; turn an LFO shape into a frame. |
| Frames | Key frames of the table. Click to select; Shift for a range, Ctrl for several. Drag to reorder. Arrow keys move, Delete removes. |
| Insert | Insert a copy of the current frame after it. |
| Duplicate | Duplicate the selected frames. |
| Delete | Delete the selected frames. |
| Thin | Keep every second (or Nth) frame. |
| Crop | Keep only the selected frames. |
| Reverse | Reverse the order of the frames. |
| Drawing shape: Free, Line, Flat, Ramp up, Ramp down, Sine, Curve | Drag across the wave. |
| More shapes | Arches, quarter curves, Nudge and Noise. |
| Snap | Strokes follow the grid. The lamp is lit while it is latched. |
| Grid 16 | Grid lines across the cycle: choose how many. |
| Wave | Draw on the frame. Pick a tool; with Snap the strokes follow the grid. Faint lines are the neighbouring frames. |
| Harmonics | Level (top) and phase (bottom) of each harmonic of the frame. Drag across the bars to draw them. Right-click for operations. |
| Paint | Spectral brush: Paint |
| Erase | Spectral brush: Erase |
| Smooth | Spectral brush: Smooth |
| Lock harmonic | The brush stays on the harmonic where the stroke began. |
| Spectral view | Frames (across) by harmonics (up), brightness is level. Paint, erase or smooth with the brush; Lock keeps the brush on one harmonic. |
| Wave | Write samples (t, x) or harmonic levels (h). |
| All frames | Which frames the formula writes. |
| Apply | Write the frames with the formula (Enter). |
| Formulas | Factory formulas and yours. |

### The PLAY page

#### Arpeggiator

| Control | Help strip |
|---|---|
| Arp On | Power switch. |
| Arp Gate | Note length as a share of the step. |
| Arp Swing | Delays every second step. |
| Arp Mode | Order in which held notes are played. |
| Arp Octaves | Number of octaves the pattern spans. |
| Arp Rate | Length of one step. |
| Arp Latch | Keeps the pattern running after the keys are released. |
| Arp Restart | Restarts the pattern when a new note starts from silence. |
| Arp Steps | Length of the step lane. |
| Arp Pattern | Which of the twelve pattern slots plays. |
| Copy pattern | Copy this step pattern. |
| Paste pattern | Paste a copied step pattern into this slot. |
| Step lane | Each step: click the top cell for on, tie or rest; drag the bar for velocity; drag the lower cells for transposition and ratchets. Arrow keys move, Space changes the state. |

#### Chords and scale

| Control | Help strip |
|---|---|
| Chord notes | The chord one key plays: click semitones above the key (0 is the key itself). Arrow keys move, Space toggles. |
| Chord Memory | Plays the stored chord from a single key. |
| Learn held keys | Hold a chord on the keyboard, then click: it becomes the chord memory (relative to its lowest key). |
| Chord Scale Lock | Keeps played notes inside the chosen key and scale. |
| Chord Key | Root note of the scale. |
| Chord Scale | Scale used by scale lock. |
| Chord Lock Mode | Snap moves outside notes to the nearest scale note; Filter drops them. |
| Chord Strum | Delay between successive notes of a chord. |
| Chord Strum Direction | Order in which the notes of a chord are strummed. |

#### Clip player

| Control | Help strip |
|---|---|
| Clip On | Power switch. |
| Clip Slot | Which of the eight clips plays. |
| Clip Trigger | Transport follows the host; Key starts the clip from a played note and transposes it. |
| Piano roll | Double-click to add a note; drag to move, drag the right edge for length, drag in the lower strip for velocity. Delete removes; the Quantize menu snaps starts. |
| Clip Latch | Keeps the clip playing after the key is released. |
| Clip Loop | Repeats the clip. |
| Import MIDI | Put a clip from the factory or your Clips folder, or any Standard MIDI File, into this clip slot (or drop a file on the page). |
| Export MIDI | Save this clip as a Standard MIDI File. |
| Drag to host | Drag from here into your host's arrangement to drop the clip as MIDI. |
| Quantize | Snap note starts (the selected notes, or all) to a grid. |
| Global MIDI Out | Sends notes made by the arpeggiator, chord tools and clip player to the host. |
| Global MIDI Thru | Passes incoming MIDI on to the host unchanged. |

### The LIBRARY page

#### On the page

| Control | Help strip |
|---|---|
| Import pack | Import a .zpack file or a folder of presets into User Presets; sounds already in the library are skipped. Dropping one on the page works too. |
| New folder | Makes a folder inside the selected folder (inside User Presets when none is selected), for a pack of your own. |
| Show in Explorer | Opens the selected folder (or the Presets folder) in Explorer. |
| Rescan | Read the preset folders again. |
| Find duplicates | Select presets whose sounds are identical. |
| Module presets | Save one module (an oscillator, filter, envelope, LFO, effect, arpeggiator pattern or clip) as a preset, or load one into a module of the same kind. |
| PRESETS, USER PRESETS, SHOW, TYPE, CHARACTER, GENRE | Expand this section. |

### The AGENT page

#### Agent link

| Control | Help strip |
|---|---|
| Agent link | Turns this instance's local link for AI agents on or off. Off by default; the top bar shows when it is on. |

#### Activity

| Control | Help strip |
|---|---|
| Revert agent session | Restores the sound from before the first agent edit since Agent Link was switched on (as one undo step). |

#### Permissions for every agent on this computer

| Control | Help strip |
|---|---|
| Read the patch | Agents may read this instance's sound. |
| Edit the patch | Agents may change this instance's sound (every change is an undo step). |
| Write in Zederia's folders | Agents may save presets, tables, clips, renders and packs in your Zederia folders. |
| Write in allowed folders | Agents may also write in the folders you allow below. |
| Audition through speakers | Agents may play notes through this instance, held below the cap. |
| Read samples in allowed folders | Agents may read audio and images in the folders you allow below. |
| Audition cap | The highest peak an agent's audition may reach. Click to step through -6, -12, -18 and -24 dB. |
| Allow a write folder..., Allow a read folder... | Chooses a folder agents may also use. |
| Clear folders | Removes every allowed folder. |

#### Agent clients

| Control | Help strip |
|---|---|
| Client setup | Choose your agent and copy a setup prompt or its configuration file entry. |

<!-- ui-help: end -->
