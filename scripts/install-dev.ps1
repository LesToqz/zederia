# Copies a built plug-in and the tools to the per-user development locations (no administrator
# rights needed; see docs/ENVIRONMENT.md):
#   VST3   %LOCALAPPDATA%\Programs\Common\VST3\Zederia.vst3
#   CLAP   %LOCALAPPDATA%\Programs\Common\CLAP\Zederia.clap
#   tools  %LOCALAPPDATA%\Programs\Zederia\  (zederia-cli, zederia-mcp, the standalone)
#   factory content  %LOCALAPPDATA%\Zederia\Factory\  (built from the recipes in content/; the
#                    per-user fallback of the factory folder, see plan 5.6). While the installer's
#                    %PROGRAMDATA%\Zederia\Factory exists, every copy of Zederia uses that one.
# Beside an installed Zederia, hosts that scan the per-user plug-in folders list both copies.
#
#   scripts/install-dev.ps1               install the Release build
#   scripts/install-dev.ps1 -Config Debug
#   scripts/install-dev.ps1 -Uninstall    remove what this script installs
# A program that is running (an agent session's zederia-mcp.exe) is renamed aside and replaced; the
# old copy is deleted by a later run once nothing uses it. Close a host that has the plug-in loaded.
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Config = 'Release',
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vst3Dir = Join-Path $env:LOCALAPPDATA 'Programs\Common\VST3'
$clapDir = Join-Path $env:LOCALAPPDATA 'Programs\Common\CLAP'
$toolsDir = Join-Path $env:LOCALAPPDATA 'Programs\Zederia'
$factoryDir = Join-Path $env:LOCALAPPDATA 'Zederia\Factory'
$installed = @(
    (Join-Path $vst3Dir 'Zederia.vst3'), (Join-Path $clapDir 'Zederia.clap'),
    (Join-Path $toolsDir 'zederia-cli.exe'), (Join-Path $toolsDir 'zederia-mcp.exe'), (Join-Path $toolsDir 'Zederia.exe'),
    $factoryDir
)

# Programs moved aside by an earlier install because they were running (see below); each goes once
# nothing runs it any more.
function Remove-SetAside {
    foreach ($dir in @($toolsDir, $clapDir)) {
        if (-not (Test-Path $dir)) { continue }
        Get-ChildItem -LiteralPath $dir -Filter '*.old-*' -File | ForEach-Object {
            try { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction Stop; "removed $($_.FullName)" }
            catch { "still in use, left for a later run: $($_.FullName)" }
        }
    }
}

if ($Uninstall) {
    foreach ($item in $installed) {
        if (Test-Path $item) { Remove-Item -Recurse -Force $item; "removed $item" }
    }
    Remove-SetAside
    return
}
Remove-SetAside

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
    if (Test-Path $target) { # a VST3 can be a folder; replace it whole
        try {
            Remove-Item -Recurse -Force $target -ErrorAction Stop
        } catch {
            # A running program (every agent session keeps its own zederia-mcp.exe open) cannot be
            # deleted, but it can be renamed: it runs on from the old copy and the next start uses
            # the new one. A plug-in folder a host has loaded has to be closed there first.
            if (Test-Path $target -PathType Container) { throw "$target is in use: close the host that has Zederia loaded, then run this again" }
            $aside = (Split-Path -Leaf $target) + '.old-' + (Get-Date -Format 'yyyyMMddHHmmss')
            Rename-Item -LiteralPath $target -NewName $aside
            "moved the running $(Split-Path -Leaf $target) aside as $aside (removed by a later install)"
        }
    }
    Copy-Item -Recurse -Force $s.From $s.To
    "installed $target"
}

# Factory wavetables and textures are built from their recipes, not copied.
$cli = Join-Path $build 'cli\zederia-cli.exe'
if (Test-Path $factoryDir) { Remove-Item -Recurse -Force $factoryDir }
& $cli content build --in (Join-Path $root 'content') --out $factoryDir | Select-Object -Last 1
if ($LASTEXITCODE -ne 0) { throw "building the factory content failed" }
"installed $factoryDir"
