# Status

## Done
- Milestone 0 (environment): toolchain verified, versions pinned, smoke build passed, repo files created.

## Measurements
- pluginval v1.0.4 (installed in %LOCALAPPDATA%ProgramsZederia	oolspluginval) on the smoke VST3 at strictness 5: SUCCESS.
- Smoke configure 9 s, build 201 s (-j 3, Release). Console and Catch2 test pass.

## Decisions
- MSVC from VS 2022 Build Tools (v143). Build with -j 3 because of 7.9 GB RAM.
- CLAP via clap-juce-extensions (pinned commit) since JUCE 9.0.5 lacks native CLAP.

## Known issues
- (none blocking)
- Inno Setup not installed (needed by milestone 7).
- FL Studio per-user plug-in folder scanning unconfirmed.
- JUCE licence choice is open.

## Next
- Milestone 1 (Foundation).
