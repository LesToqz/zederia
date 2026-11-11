# Dot-sourced by the other scripts: puts the MSVC x64 toolchain on the path (once per session)
# and moves to the repository root.
$ErrorActionPreference = 'Stop'
$script:RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $script:RepoRoot

if (-not $env:VSCMD_VER) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; run scripts/check-env.ps1" }
    $need = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
    # Prefer the pinned toolset generation (v143, Visual Studio 2022); fall back to the newest.
    $vs = & $vswhere -products * -version '[17.0,18.0)' -latest -requires $need -property installationPath
    if (-not $vs) { $vs = & $vswhere -products * -latest -requires $need -property installationPath }
    if (-not $vs) { throw "No Visual C++ toolset found; run scripts/check-env.ps1" }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
    }
    if (-not $env:VSCMD_VER) { throw "Could not load the Visual C++ environment from $vcvars" }
}

function Invoke-Checked {
    # Runs a native command and stops the script when it fails.
    param([Parameter(Mandatory)][string]$Exe, [Parameter(ValueFromRemainingArguments)][string[]]$Rest)
    & $Exe @Rest
    if ($LASTEXITCODE -ne 0) { throw "$Exe $($Rest -join ' ') failed with exit code $LASTEXITCODE" }
}
