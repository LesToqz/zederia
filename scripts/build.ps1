# Configures and builds one preset.
#   scripts/build.ps1                 Release
#   scripts/build.ps1 -Config Debug
#   scripts/build.ps1 -Clean          start from an empty build folder
#   scripts/build.ps1 -Target zederia-cli
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Config = 'Release',
    [switch]$Clean,
    [string]$Target = '',
    [int]$Jobs = 3 # the development machine has 8 GB of memory; see docs/ENVIRONMENT.md
)
. "$PSScriptRoot\_env.ps1"
$preset = $Config.ToLower()
$dir = Join-Path $RepoRoot "build\$preset"
if ($Clean -and (Test-Path $dir)) { Remove-Item -Recurse -Force $dir }

$timer = [Diagnostics.Stopwatch]::StartNew()
Invoke-Checked cmake --preset $preset
$configureSeconds = [math]::Round($timer.Elapsed.TotalSeconds)
$timer.Restart()
$buildArgs = @('--build', '--preset', $preset, '-j', "$Jobs")
if ($Target) { $buildArgs += @('--target', $Target) }
Invoke-Checked cmake @buildArgs
"Built $Config in build\$preset (configure $configureSeconds s, build $([math]::Round($timer.Elapsed.TotalSeconds)) s)"
