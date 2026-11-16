# Builds one preset and runs every test.
#   scripts/test.ps1                      Release
#   scripts/test.ps1 -Config Debug
#   scripts/test.ps1 -Filter tuning       only tests whose name matches
#   scripts/test.ps1 -Pluginval           also validate the VST3 with pluginval at strictness 10
#   scripts/test.ps1 -ClapValidator       also validate the CLAP build with clap-validator
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Config = 'Release',
    [string]$Filter = '',
    [switch]$Pluginval,
    [switch]$ClapValidator,
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

if ($ClapValidator) {
    $validator = Join-Path $env:LOCALAPPDATA 'Programs\Zederia\tools\clap-validator\clap-validator.exe'
    if (-not (Test-Path $validator)) { throw "clap-validator not found at $validator (see docs/ENVIRONMENT.md)" }
    $clap = Join-Path $RepoRoot "build\$preset\plugin\Zederia_artefacts\$Config\CLAP\Zederia.clap"
    $log = Join-Path $RepoRoot "build\$preset\clap-validator.log"
    # Start-Process: the validator logs to standard error, which this shell would treat as failure.
    $run = Start-Process -FilePath $validator -Wait -PassThru -NoNewWindow -RedirectStandardOutput $log `
        -RedirectStandardError "$log.err" -ArgumentList @('validate', "`"$clap`"", '--hide-output')
    Get-Content $log | Select-Object -Last 1
    if ($run.ExitCode -ne 0) { throw "clap-validator failed with exit code $($run.ExitCode); full log: $log" }
    "clap-validator: SUCCESS (log: $log)"
}
