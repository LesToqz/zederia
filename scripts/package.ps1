# Builds the installer build\package\ZederiaSetup-<version>.exe with Inno Setup 7 (installer/zederia.iss):
# stages the Release plug-ins and tools, builds the factory content from the recipes in content/,
# and compiles the installer with the version of the build.
#   scripts/package.ps1             build Release first
#   scripts/package.ps1 -NoBuild    package the Release build as it is
# Inno Setup is not installed by this script (see docs/ENVIRONMENT.md).
param(
    [switch]$NoBuild,
    [string]$Iscc = ''
)
. "$PSScriptRoot\_env.ps1"
if (-not $NoBuild) { & "$PSScriptRoot\build.ps1" -Config Release }

function Find-Iscc {
    if ($Iscc) { return $Iscc }
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 7\ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 7\ISCC.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 7\ISCC.exe')
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    $onPath = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    throw 'Inno Setup 7 (ISCC.exe) was not found: install it from https://jrsoftware.org/isdl.php, or pass -Iscc <path>'
}
$compiler = Find-Iscc

$build = Join-Path $RepoRoot 'build\release'
$artefacts = Join-Path $build 'plugin\Zederia_artefacts\Release'
$cli = Join-Path $build 'cli\zederia-cli.exe'
$package = Join-Path $RepoRoot 'build\package'
$stage = Join-Path $package 'stage'

# The version comes from the build itself ("zederia-cli 1.0.0-rc1"), so the installer and the
# binaries cannot disagree.
$versionLine = (& $cli --version | Select-Object -First 1)
if ($versionLine -notmatch '^zederia-cli (\d+)\.(\d+)\.(\d+)(\S*)$') { throw "cannot read the version from '$versionLine'" }
$version = "$($Matches[1]).$($Matches[2]).$($Matches[3])$($Matches[4])"
$numeric = "$($Matches[1]).$($Matches[2]).$($Matches[3]).0"

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage, (Join-Path $stage 'VST3'), (Join-Path $stage 'CLAP'), (Join-Path $stage 'docs') | Out-Null
$copies = @(
    @{ From = (Join-Path $artefacts 'VST3\Zederia.vst3'); To = (Join-Path $stage 'VST3') },
    @{ From = (Join-Path $artefacts 'CLAP\Zederia.clap'); To = (Join-Path $stage 'CLAP') },
    @{ From = (Join-Path $artefacts 'Standalone\Zederia.exe'); To = $stage },
    @{ From = (Join-Path $build 'mcp\zederia-mcp.exe'); To = $stage },
    @{ From = $cli; To = $stage },
    @{ From = (Join-Path $RepoRoot 'plugin\brand\zederia.ico'); To = $stage }
)
foreach ($c in $copies) {
    if (-not (Test-Path $c.From)) { throw "$($c.From) not found; run scripts/build.ps1 first" }
    Copy-Item -Recurse -Force $c.From $c.To
}
Copy-Item -Force (Join-Path $RepoRoot 'LICENSE') (Join-Path $stage 'LICENSE.txt')
Copy-Item -Force (Join-Path $RepoRoot 'NOTICE') (Join-Path $stage 'NOTICE.txt')
# The documents, with the logo beside them: in the source tree they show it from plugin\brand\png.
New-Item -ItemType Directory -Force (Join-Path $stage 'docs\images') | Out-Null
Copy-Item -Force (Join-Path $RepoRoot 'plugin\brand\png\zederia-128.png') (Join-Path $stage 'docs\images\zederia-128.png')
$utf8 = New-Object System.Text.UTF8Encoding($false)
foreach ($doc in @('README.md', 'docs\MANUAL.md', 'docs\AGENT_GUIDE.md', 'docs\RELEASE_NOTES.md', 'docs\PARAMETERS.md')) {
    $from = Join-Path $RepoRoot $doc
    if (-not (Test-Path $from)) { throw "$doc is missing" }
    $text = [System.IO.File]::ReadAllText($from) -replace '(\.\./)?plugin/brand/png/', 'images/'
    [System.IO.File]::WriteAllText((Join-Path $stage "docs\$(Split-Path -Leaf $doc)"), $text, $utf8)
}

# The factory content is built from its recipes and checked, like the development install.
& $cli content build --in (Join-Path $RepoRoot 'content') --out (Join-Path $stage 'Factory') | Select-Object -Last 1
if ($LASTEXITCODE -ne 0) { throw 'building the factory content failed' }

$script = Join-Path $RepoRoot 'installer\zederia.iss'
& $compiler /Q "/DAppVersion=$version" "/DNumericVersion=$numeric" "/DStage=$stage" "/DOutputDir=$package" $script
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed with exit code $LASTEXITCODE" }
$setup = Join-Path $package "ZederiaSetup-$version.exe"
if (-not (Test-Path $setup)) { throw "$setup was not made" }
$size = [math]::Round((Get-Item $setup).Length / 1MB, 1)
"Made $setup ($size MB, version $version)"
