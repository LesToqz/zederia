# Environment

Verified 2026-11-10 with `scripts/check-env.ps1` (re-runnable; exits non-zero if a required tool is missing).

## Machine
- Windows 11 Pro 10.0.26300; AMD Ryzen 7 8845HS (AVX2: yes); 7.9 GB RAM; about 37 GB free on C:.
- Audio: High Definition Audio Device.
- RAM is tight: build with at most 2-3 parallel jobs (`-j 3`).

## Toolchain
| Tool | Version |
|---|---|
| Git | 2.56.0 |
| CMake | 4.4.4 |
| Ninja | 1.13.2 |
| MSVC | VS 2022 Build Tools 17.14 (v143), C++20 compile and run verified |
| Windows SDK | 10.0.26100.0 |
| Also present | VS 2026 Insiders (not used) |

Add `/Zc:__cplusplus` to MSVC flags, otherwise `__cplusplus` reads 199711 (C++20 itself works).
Optional, not installed: clang-cl, Inno Setup (needed for milestone 7), claude, codex, agy. Node is present. Host found: FL Studio.

## Pinned dependencies (cmake/Versions.cmake)
| Dependency | Pin |
|---|---|
| JUCE | 9.0.5 |
| Catch2 | v3.16.1 |
| nlohmann/json | v3.12.0 |
| cpp-httplib | v0.65.0 |
| pluginval (tool) | v1.0.4 |
| clap-validator (tool) | 0.4.1 |
| CLAP route | clap-juce-extensions at commit 6bde108 (JUCE 9.0.5 has no native CLAP client); CLAP SDK tag 1.2.10 to be confirmed in milestone 1 |

## JUCE licence options (checked 2026-11-10, juce.com/get-juce)
- Starter: free, revenue up to $20,000 per year.
- Indie: $40 per user per month or $800 one-time; revenue up to $300,000 per year.
- Pro: $175 per user per month or $3,500 one-time; no revenue limit.
- AGPLv3 is also available if the product's source is released under it.
The choice is yours and is needed before distribution.

## Smoke test (cmake/smoke, built in build/_smoke)
Plug-in (VST3 and Standalone), console program and Catch2 test all built and ran in Release. Configure 9 s (first run 108 s including the JUCE fetch), build 201 s with 3 jobs.

## Validator
pluginval v1.0.4 is in `%LOCALAPPDATA%ProgramsZederia	oolspluginvalpluginval.exe`; use `--validate-in-process`.

## Development install locations (no administrator rights)
- VST3: `%LOCALAPPDATA%\Programs\Common\VST3\`
- CLAP: `%LOCALAPPDATA%\Programs\Common\CLAP\`
- Tools: `%LOCALAPPDATA%\Programs\Zederia\`
These folders do not exist yet. Whether FL Studio scans them by default is not confirmed: the VST3 standard includes the per-user folder, but FL Studio's extra search paths must be checked in its plug-in manager settings.
