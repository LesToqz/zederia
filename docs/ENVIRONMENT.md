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
Optional, not installed: clang-cl, Inno Setup (needed for milestone 7), agy. Node is present. Host found: FL Studio. The agent clients claude and codex were installed later (see Agent clients below).

## Pinned dependencies (cmake/Versions.cmake)
| Dependency | Pin |
|---|---|
| JUCE | 9.0.5 |
| Catch2 | v3.16.1 |
| nlohmann/json | v3.12.0 |
| cpp-httplib | v0.65.0 (fetched from milestone 6 on; header-only, used without TLS) |
| pluginval (tool) | v1.0.4 |
| clap-validator (tool) | 0.4.1 |
| CLAP route | clap-juce-extensions at commit 6bde108 (JUCE 9.0.5 has no native CLAP client); it brings CLAP SDK 1.2.7 as a submodule (confirmed in milestone 1: the CLAP target builds) |

## JUCE licence options (checked 2026-11-10, juce.com/get-juce)
- Starter: free, revenue up to $20,000 per year.
- Indie: $40 per user per month or $800 one-time; revenue up to $300,000 per year.
- Pro: $175 per user per month or $3,500 one-time; no revenue limit.
- AGPLv3 is also available if the product's source is released under it.
The choice is yours and is needed before distribution.

## Smoke test (cmake/smoke, built in build/_smoke)
Plug-in (VST3 and Standalone), console program and Catch2 test all built and ran in Release. Configure 9 s (first run 108 s including the JUCE fetch), build 201 s with 3 jobs.

## Validators
pluginval v1.0.4 is in `%LOCALAPPDATA%\Programs\Zederia\tools\pluginval\pluginval.exe`; use `--validate-in-process` (`scripts/test.ps1 -Pluginval` does this at strictness 10).

clap-validator 0.4.1 (the Windows build of its release `0.4.1-127-g152b982`, installed in milestone 4) is in `%LOCALAPPDATA%\Programs\Zederia\tools\clap-validator\clap-validator.exe`; `scripts/test.ps1 -ClapValidator` runs `validate` on the built CLAP and writes the log to `build\<preset>\clap-validator.log`. It logs to standard error, so the script starts it with `Start-Process` and redirects both streams.

## Agent clients (checked 2026-11-19, milestone 6)
Which protocol revisions each installed client speaks was observed with a logging test server, not taken from documentation:

| Client | Version | Where | Speaks |
|---|---|---|---|
| Claude Code | 2.1.322 | `%USERPROFILE%\.local\bin\claude.exe` | probes with `server/discover` at 2026-07-28 and stays on it when the server answers; falls back to `initialize` at 2025-11-25 otherwise. The command-line program is not signed in on this machine, so it cannot run prompts from a script here (`claude mcp list` still connects) |
| Codex CLI | 0.170.0 | `%LOCALAPPDATA%\Programs\OpenAI\Codex\bin\codex.exe` | `initialize` at 2025-06-18 only |
| Antigravity CLI (`agy`) | - | not installed | - |

`zederia-mcp` is registered with Claude Code for this project only (`claude mcp add -s local zederia -- "%LOCALAPPDATA%/Programs/Zederia/zederia-mcp.exe" --stdio`, stored in `%USERPROFILE%\.claude.json` under this folder; `claude mcp remove -s local zederia` removes it). Codex was given the server per run with `-c mcp_servers.zederia.command=...`; nothing was written to `~/.codex/config.toml`.

## Development install locations (no administrator rights)
- VST3: `%LOCALAPPDATA%\Programs\Common\VST3\`
- CLAP: `%LOCALAPPDATA%\Programs\Common\CLAP\`
- Tools: `%LOCALAPPDATA%\Programs\Zederia\`
These folders do not exist yet. Whether FL Studio scans them by default is not confirmed: the VST3 standard includes the per-user folder, but FL Studio's extra search paths must be checked in its plug-in manager settings.
