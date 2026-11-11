# Copies a built plug-in and the tools to the per-user development locations (no administrator
# rights needed; see docs/ENVIRONMENT.md):
#   VST3   %LOCALAPPDATA%\Programs\Common\VST3\Zederia.vst3
#   CLAP   %LOCALAPPDATA%\Programs\Common\CLAP\Zederia.clap
#   tools  %LOCALAPPDATA%\Programs\Zederia\  (zederia-cli, zederia-mcp, the standalone)
#
#   scripts/install-dev.ps1               install the Release build
#   scripts/install-dev.ps1 -Config Debug
#   scripts/install-dev.ps1 -Uninstall    remove what this script installs
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Config = 'Release',
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vst3Dir = Join-Path $env:LOCALAPPDATA 'Programs\Common\VST3'
$clapDir = Join-Path $env:LOCALAPPDATA 'Programs\Common\CLAP'
$toolsDir = Join-Path $env:LOCALAPPDATA 'Programs\Zederia'
$installed = @(
    (Join-Path $vst3Dir 'Zederia.vst3'), (Join-Path $clapDir 'Zederia.clap'),
    (Join-Path $toolsDir 'zederia-cli.exe'), (Join-Path $toolsDir 'zederia-mcp.exe'), (Join-Path $toolsDir 'Zederia.exe')
)

if ($Uninstall) {
    foreach ($item in $installed) {
        if (Test-Path $item) { Remove-Item -Recurse -Force $item; "removed $item" }
    }
    return
}

$build = Join-Path $root "build\$($Config.ToLower())"
$artefacts = Join-Path $build "plugin\Zederia_artefacts\$Config"
$sources = @(
    @{ From = (Join-Path $artefacts 'VST3\Zederia.vst3'); To = $vst3Dir; Required = $true },
    @{ From = (Join-Path $artefacts 'CLAP\Zederia.clap'); To = $clapDir; Required = $false },
    @{ From = (Join-Path $artefacts 'Standalone\Zederia.exe'); To = $toolsDir; Required = $true },
    @{ From = (Join-Path $build 'cli\zederia-cli.exe'); To = $toolsDir; Required = $true },
    @{ From = (Join-Path $build 'mcp\zederia-mcp.exe'); To = $toolsDir; Required = $true }
)
foreach ($s in $sources) {
    if (-not (Test-Path $s.From)) {
        if ($s.Required) { throw "$($s.From) not found; run scripts/build.ps1 -Config $Config first" }
        "skipped (not built): $($s.From)"; continue
    }
    New-Item -ItemType Directory -Force $s.To | Out-Null
    $target = Join-Path $s.To (Split-Path -Leaf $s.From)
    if (Test-Path $target) { Remove-Item -Recurse -Force $target } # a VST3 can be a folder; replace it whole
    Copy-Item -Recurse -Force $s.From $s.To
    "installed $target"
}
