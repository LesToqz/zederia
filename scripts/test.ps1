# Builds one preset and runs every test.
#   scripts/test.ps1                      Release
#   scripts/test.ps1 -Config Debug
#   scripts/test.ps1 -Filter tuning       only tests whose name matches
#   scripts/test.ps1 -Pluginval           also validate the VST3 with pluginval at strictness 10
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Config = 'Release',
    [string]$Filter = '',
    [switch]$Pluginval,
    [switch]$NoBuild
)
. "$PSScriptRoot\_env.ps1"
$preset = $Config.ToLower()
if (-not $NoBuild) { & "$PSScriptRoot\build.ps1" -Config $Config }

$ctestArgs = @('--preset', $preset, '-j', '3')
if ($Filter) { $ctestArgs += @('-R', $Filter) }
Invoke-Checked ctest @ctestArgs

if ($Pluginval) {
    $validator = Join-Path $env:LOCALAPPDATA 'Programs\Zederia\tools\pluginval\pluginval.exe'
    if (-not (Test-Path $validator)) { throw "pluginval not found at $validator (see docs/ENVIRONMENT.md)" }
    $vst3 = Join-Path $RepoRoot "build\$preset\plugin\Zederia_artefacts\$Config\VST3\Zederia.vst3"
    $log = Join-Path $RepoRoot "build\$preset\pluginval.log"
    # Start-Process -Wait: pluginval is a GUI-subsystem program and would otherwise return at once.
    $run = Start-Process -FilePath $validator -Wait -PassThru -NoNewWindow -RedirectStandardOutput $log `
        -ArgumentList @('--strictness-level', '10', '--validate-in-process', '--validate', "`"$vst3`"")
    Get-Content $log | Select-Object -Last 12
    if ($run.ExitCode -ne 0) { throw "pluginval failed with exit code $($run.ExitCode); full log: $log" }
    "pluginval strictness 10: SUCCESS (log: $log)"
}
