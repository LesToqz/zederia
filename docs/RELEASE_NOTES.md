# Zederia 1.0.1 release notes

Zederia, a wavetable synthesizer for Windows (VST3, CLAP and a Standalone application) with a
local bridge for AI agents. Please report what does not work.

## Changes since 1.0.0-rc1

- The installer shows the Zederia logo when Windows is in dark mode too; it showed Inno Setup's
  own picture there.
- The installer also copies the factory presets into `Documents\Zederia\Presets`, in their
  folders (`Bass`, `Lead`, `Pad`, ...), so they can be seen and used in Explorer. The library
  lists each of them once. A copy you change is yours: upgrades do not replace it and the
  uninstaller keeps it.

## What is in it

- The instrument: three wavetable or classic oscillators with warps and unison, a Sub and a
  Texture player, two filters per voice (70 types), four envelopes, eight LFOs, two random
  generators, eight macros, a 64-route modulation matrix, thirteen effects in three lanes, an
  arpeggiator, chord tools and a clip player. See [MANUAL.md](MANUAL.md).
- Factory content: 184 presets (808, Arp, Bass, FX, Keys, Lead, Pad, Pluck and Synth, and the
  Default preset a new instance starts with), 62 wavetables, 21 textures, 24 LFO shapes, 16
  clips, 12 effect racks and 8 single-effect presets. Every preset passed the content check:
  it loads, plays its audition phrase without clipping or silence, and stays within the CPU
  budget.
- Agent tools: `zederia-mcp` (26 tools over the Model Context Protocol, revisions 2026-07-28,
  2025-11-25 and 2025-06-18, on standard input and output or loopback HTTP) and `zederia-cli`.
  See [AGENT_GUIDE.md](AGENT_GUIDE.md).

## System requirements

Windows 10 or 11, 64-bit, on an x64 processor (AVX2 is used when present). A VST3 or CLAP host,
or the Standalone. About 90 MB of disk space.

## Installing

1. Download `ZederiaSetup-1.0.1.exe`. A browser may say that it is not commonly downloaded:
   choose to keep it.
2. Run it. **The installer is not code-signed** (signing needs a certificate that has not been
   bought for this release), so Windows Defender SmartScreen may stop it first with a blue
   window, **"Windows protected your PC"**: "Microsoft Defender SmartScreen prevented an
   unrecognized app from starting. Running this app might put your PC at risk." To continue,
   click **More info**: the window then shows the app's name (`ZederiaSetup-1.0.1.exe`) and
   **Publisher: Unknown publisher**. Click **Run anyway**.
3. Windows then asks for administrator rights ("Do you want to allow this app from an unknown
   publisher to make changes to your device?"). Click **Yes**. This is the only time the
   installer needs them.
4. Follow the pages: welcome, the licence (GNU AGPL v3), the parts to install (VST3, CLAP,
   Standalone, agent tools; the factory content is always installed), and Install.

Before running an unsigned installer, check that it is the file that was published: in
PowerShell, `Get-FileHash .\ZederiaSetup-1.0.1.exe -Algorithm SHA256` must print the
SHA-256 value published beside the download. If **Smart App Control** is on (Windows 11), it
blocks unsigned installers with no Run anyway button; the installer cannot be run while it is
on.

What goes where:

| Part | Folder |
|---|---|
| VST3 plug-in | `C:\Program Files\Common Files\VST3\Zederia.vst3` |
| CLAP plug-in | `C:\Program Files\Common Files\CLAP\Zederia.clap` |
| Standalone, `zederia-mcp`, `zederia-cli`, documents | `C:\Program Files\Zederia` |
| Factory content | `%PROGRAMDATA%\Zederia\Factory` |
| A copy of the factory presets, for the user who installs | `Documents\Zederia\Presets` |
| Your presets and files (made when first used) | `Documents\Zederia` |

Hosts find the plug-ins in these standard folders. If FL Studio does not list Zederia yet,
choose **ADD > Refresh plugin list (fast scan)** once; Zederia then appears in the ADD menu and
the plug-in database under its own name.

## Upgrading and uninstalling

- **Upgrading**: run the new installer over the old version. It removes the files the earlier
  version installed (plug-ins, programs, factory content) before installing, so no stale files
  stay, and keeps your own files.
- **Uninstalling**: **Settings > Apps > Installed apps > Zederia > Uninstall** (or Apps &
  features on Windows 10). It removes everything the installer put on the computer, including
  the Start menu entry and its registry key. It keeps `Documents\Zederia` (your presets,
  wavetables, clips, renders and packs), your settings and the agent permissions in
  `%APPDATA%\Zederia`, and `%LOCALAPPDATA%\Zederia`. Delete those folders yourself to remove
  every trace.
- **A development install** (`scripts\install-dev.ps1` from the source tree) puts a second copy
  of the plug-ins in the per-user folders `%LOCALAPPDATA%\Programs\Common\VST3` and `CLAP`;
  hosts that scan both list Zederia twice. Remove it with `scripts\install-dev.ps1 -Uninstall`.
  The installer says so when it finds one. While the machine-wide factory folder exists, it is
  used instead of the development install's per-user one.

## Known issues

- The installer and the programs are not code-signed (see Installing).
- Hosts checked for 1.0.0-rc1: FL Studio 2026 (26.1.1) with the installed VST3 (loading,
  playing, two instances at once, automating a parameter, saving and reopening a project,
  rendering it) and the Standalone (loading, playing, the factory library, the agent link).
  The CLAP plug-in passed its validator but was not loaded in a host, and no other host was
  tried.
- Nobody has listened to every factory preset in a host; the content check measures them.
- In a host, the computer keys play only while Zederia's window has the keyboard focus, and some
  hosts take letters for their own typing keyboard first.
- The undo history is not saved with the host project.
- Texture samples are not embedded in presets; a preset that uses your own sample needs the file.
- MPE: only the lower zone is handled, and the play features send their notes on channel 1.
- The envelope follower's side-chain source hears silence in a host (there is no side-chain bus
  yet).
- File paths with characters outside the system's code page are not handled for tables,
  textures, tunings and program lists.
- The live link for agents works on Windows only, through named pipes that only the current user
  can open.

## Source code and licence

Zederia is free software: you can redistribute it and modify it under the terms of the GNU
Affero General Public License as published by the Free Software Foundation, version 3 or (at
your option) any later version. It comes with no warranty. The licence is in `LICENSE.txt`
beside the programs, and the third-party components (JUCE, the VST3 SDK, the CLAP SDK and its
JUCE extensions, nlohmann/json, cpp-httplib, and the Inno Setup setup program) are listed with
their licences in `NOTICE.txt`.

The complete corresponding source code of this release is the Zederia source tree at the tag
`v1.0.1`. Whoever passes this installer on must also offer that source, as the licence's
section 6 requires.
