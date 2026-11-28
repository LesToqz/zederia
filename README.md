<p align="center"><img src="plugin/brand/png/zederia-128.png" width="96" alt="Zederia logo: a Z cut into two slabs"></p>

# Zederia

A wavetable synthesizer for Windows: VST3 and CLAP plug-ins and a Standalone application, with
a local bridge that lets AI agents design sounds with it.

- Three wavetable oscillators (up to 256 frames, five morph modes, two warp slots, 16-voice
  unison) or a low-cost classic engine, a Sub and a Texture sample player.
- Two filters per voice with 70 types in 16 families, serial to parallel.
- Four envelopes, eight drawn LFOs, two random generators, eight macros and a 64-route
  modulation matrix: drag a source onto any control.
- Three effect lanes of up to eight effects each, from thirteen effects.
- An arpeggiator with a step lane, chord memory, scale lock, strum and a clip player.
- A wavetable workshop: draw, import audio or images, process, morph, sort and resynthesize.
- Factory content: 184 presets, 62 wavetables, 21 textures, 24 LFO shapes, 16 clips and 20
  effect rack presets.
- `zederia-mcp`, a Model Context Protocol server for AI agents, and `zederia-cli` for
  rendering, analysis and the wavetable toolkit.

## Download

**[Download ZederiaSetup-1.0.1.exe](https://github.com/LesToqz/zederia/releases/download/v1.0.1/ZederiaSetup-1.0.1.exe)**
(Windows 10 / 11, 64-bit, 34 MB). Every version is on the
[releases page](https://github.com/LesToqz/zederia/releases).

## Install

Run `ZederiaSetup-<version>.exe` (administrator rights once). It puts the plug-ins in the
standard folders under `C:\Program Files\Common Files`, the Standalone and the tools in
`C:\Program Files\Zederia` and the factory content in `%PROGRAMDATA%\Zederia\Factory`, with a
copy of the factory presets in `Documents\Zederia\Presets`. The
installer is not code-signed; [docs/RELEASE_NOTES.md](docs/RELEASE_NOTES.md) says what Windows
shows and how to continue.

## Documents

- [docs/MANUAL.md](docs/MANUAL.md): the manual, page by page.
- [docs/AGENT_GUIDE.md](docs/AGENT_GUIDE.md): the guide for AI agents, with a cookbook.
- [docs/RELEASE_NOTES.md](docs/RELEASE_NOTES.md): this release, installing and known issues.
- [docs/PARAMETERS.md](docs/PARAMETERS.md): every parameter (generated).
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/STATUS.md](docs/STATUS.md),
  [docs/ENVIRONMENT.md](docs/ENVIRONMENT.md) and [BUILD.md](BUILD.md): for development.

## Build from source

Windows 11 with Visual Studio 2022 Build Tools (MSVC v143), CMake and Ninja; dependencies are
fetched at pinned versions ([docs/ENVIRONMENT.md](docs/ENVIRONMENT.md)).

```powershell
scripts\build.ps1                  # Release build in build\release
scripts\test.ps1                   # build and run every test
scripts\install-dev.ps1            # per-user development install (no administrator rights)
scripts\package.ps1                # the installer, with Inno Setup 7
```

## Licence

Zederia is free software under the GNU Affero General Public License, version 3 or later
([LICENSE](LICENSE)). Third-party components and their licences are listed in [NOTICE](NOTICE).
