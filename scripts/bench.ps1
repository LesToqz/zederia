# Runs the benchmark scenarios on the Release build and compares them with the stored baseline
# (tests/bench/baseline.json). Fails when a scenario is over its budget or more than 10 % slower
# than the baseline.
#   scripts/bench.ps1
#   scripts/bench.ps1 -Scenario one_voice
#   scripts/bench.ps1 -UpdateBaseline     store the measured results as the new baseline
param(
    [string]$Scenario = 'all',
    [double]$Seconds = 20,
    [int]$Runs = 5,
    [switch]$UpdateBaseline,
    [switch]$NoBuild
)
. "$PSScriptRoot\_env.ps1"
if (-not $NoBuild) { & "$PSScriptRoot\build.ps1" -Config Release -Target zederia-cli }
$cli = Join-Path $RepoRoot 'build\release\cli\zederia-cli.exe'
$baseline = Join-Path $RepoRoot 'tests\bench\baseline.json'
$benchArgs = @('bench', '--scenario', $Scenario, '--seconds', "$Seconds", '--runs', "$Runs", '--baseline', $baseline)
if ($UpdateBaseline) { $benchArgs += '--update-baseline' }
Invoke-Checked $cli @benchArgs
