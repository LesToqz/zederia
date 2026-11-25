# Tests the installer end to end on this computer: an older version with files that later
# versions no longer ship, the upgrade to the current one, a smaller choice of parts, the
# uninstaller (nothing left, your own files kept) and a final install of the current version.
#   scripts/test-installer.ps1                 needs scripts/package.ps1 first; asks for
#                                              administrator rights once (the Windows prompt)
#   scripts/test-installer.ps1 -NoReinstall    leave Zederia uninstalled at the end
# Close hosts that have Zederia loaded and the Standalone first. The results are written to
# build\package\installer-test.log.
param(
    [switch]$NoReinstall,
    [switch]$Elevated,
    [string]$Log = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$package = Join-Path $root 'build\package'
if (-not $Log) { $Log = Join-Path $package 'installer-test.log' }

function Get-Version {
    $cli = Join-Path $package 'stage\zederia-cli.exe'
    $line = (& $cli --version | Select-Object -First 1)
    if ($line -notmatch '^zederia-cli (\S+)$') { throw "cannot read the version from '$line'" }
    return $Matches[1]
}

if (-not $Elevated) {
    # Not elevated: make the older installer here (no rights needed), then run the rest elevated.
    . "$PSScriptRoot\_env.ps1"
    $version = Get-Version
    $setup = Join-Path $package "ZederiaSetup-$version.exe"
    if (-not (Test-Path $setup)) { throw "$setup not found: run scripts/package.ps1 first" }
    $oldStage = Join-Path $package 'stage-old'
    if (Test-Path $oldStage) { Remove-Item -Recurse -Force $oldStage }
    Copy-Item -Recurse (Join-Path $package 'stage') $oldStage
    # Files an older version had and this one does not: an upgrade must remove them.
    New-Item -ItemType Directory -Force (Join-Path $oldStage 'Factory\Presets\Retired') | Out-Null
    Set-Content -Encoding ascii (Join-Path $oldStage 'Factory\Presets\Retired\Old Sound.zpreset') '{"format":"zederia.patch"}'
    Set-Content -Encoding ascii (Join-Path $oldStage 'Factory\Wavetables\Basics\Old Table.wav') 'old'
    Set-Content -Encoding ascii (Join-Path $oldStage 'docs\OLD_NOTES.md') 'old'
    Set-Content -Encoding ascii (Join-Path $oldStage 'VST3\Zederia.vst3\Contents\Resources\old.txt') 'old'
    $iscc = @((Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 7\ISCC.exe'), (Join-Path $env:ProgramFiles 'Inno Setup 7\ISCC.exe')) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) { throw 'Inno Setup 7 (ISCC.exe) was not found' }
    $oldOut = Join-Path $package 'old'
    & $iscc /Q '/DAppVersion=0.9.0-test' '/DNumericVersion=0.9.0.0' "/DStage=$oldStage" "/DOutputDir=$oldOut" (Join-Path $root 'installer\zederia.iss')
    if ($LASTEXITCODE -ne 0) { throw "compiling the older installer failed ($LASTEXITCODE)" }
    $self = $MyInvocation.MyCommand.Path
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$self`"", '-Elevated', '-Log', "`"$Log`"")
    if ($NoReinstall) { $arguments += '-NoReinstall' }
    if (Test-Path $Log) { Remove-Item -Force $Log }
    $run = Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -PassThru -ArgumentList $arguments
    if (Test-Path $Log) { Get-Content $Log }
    if ($run.ExitCode -ne 0) { throw "the installer test failed (exit code $($run.ExitCode)); see $Log" }
    return
}

# --- Elevated from here --------------------------------------------------------------------------
$lines = New-Object System.Collections.Generic.List[string]
$failures = 0
function Say([string]$text) { $lines.Add($text); [System.IO.File]::WriteAllLines($Log, $lines) }
function Check([string]$what, [bool]$ok) {
    if ($ok) { Say "ok    $what" } else { Say "FAIL  $what"; $script:failures++ }
}

$version = Get-Version
$setup = Join-Path $package "ZederiaSetup-$version.exe"
$oldSetup = Join-Path $package 'old\ZederiaSetup-0.9.0-test.exe'
$appDir = Join-Path $env:ProgramFiles 'Zederia'
$vst3 = Join-Path $env:CommonProgramFiles 'VST3\Zederia.vst3'
$clap = Join-Path $env:CommonProgramFiles 'CLAP\Zederia.clap'
$factory = Join-Path $env:ProgramData 'Zederia\Factory'
$startMenu = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\Zederia.lnk'
$uninstallKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{9539CA56-225D-4CE5-A412-00AAEA93F8E2}_is1'
$userDir = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Zederia'
$settingsDir = Join-Path $env:APPDATA 'Zederia'

# What belongs to the user before anything is installed: it must be the same at the end.
function Get-Fingerprint([string]$dir) {
    if (-not (Test-Path $dir)) { return 'missing' }
    $files = Get-ChildItem -LiteralPath $dir -Recurse -File -Force | Sort-Object FullName
    return "$($files.Count) files, $((($files | Measure-Object Length -Sum).Sum)) bytes"
}
$userBefore = Get-Fingerprint $userDir
$settingsBefore = Get-Fingerprint $settingsDir

function Install([string]$file, [string]$name, [string]$extra = '') {
    $logFile = Join-Path $package "setup-$name.log"
    $arguments = "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=`"$logFile`" $extra"
    $p = Start-Process -FilePath $file -ArgumentList $arguments -Wait -PassThru
    Check "$name installer exits with 0 (log: $logFile)" ($p.ExitCode -eq 0)
}
function Uninstall {
    $u = Join-Path $appDir 'unins000.exe'
    if (-not (Test-Path $u)) { Check 'uninstaller present' $false; return }
    Start-Process -FilePath $u -ArgumentList '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' -Wait | Out-Null
    # The uninstaller runs on from a copy of itself: wait until its entry is gone.
    $until = (Get-Date).AddSeconds(180)
    while ((Test-Path $uninstallKey) -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 500 }
    Start-Sleep -Seconds 2
    Check 'uninstaller finished within 3 minutes' (-not (Test-Path $uninstallKey))
}
function Count([string]$dir, [string]$filter) { (Get-ChildItem -LiteralPath $dir -Recurse -File -Filter $filter -ErrorAction SilentlyContinue | Measure-Object).Count }

Say "Installer test, $(Get-Date -Format s), version $version"
# An earlier install is uninstalled first; copies put there by hand are noted (the test replaces them).
if (Test-Path $uninstallKey) {
    Say "      an installed Zederia $((Get-ItemProperty $uninstallKey).DisplayVersion) was found: uninstalling it first"
    Uninstall
}
foreach ($there in @($appDir, $vst3, $clap, $factory)) {
    if (Test-Path $there) { Say "      found before the test, not from an installer: $there (replaced by the test)" }
}

# 1. The older version.
Install $oldSetup 'old'
Check 'old: stale preset installed' (Test-Path (Join-Path $factory 'Presets\Retired\Old Sound.zpreset'))
Check 'old: Apps entry says 0.9.0-test' ((Get-ItemProperty $uninstallKey).DisplayVersion -eq '0.9.0-test')

# 2. The upgrade.
Install $setup 'upgrade'
Check 'upgrade: the retired preset folder is gone' (-not (Test-Path (Join-Path $factory 'Presets\Retired')))
Check 'upgrade: the retired table is gone' (-not (Test-Path (Join-Path $factory 'Wavetables\Basics\Old Table.wav')))
Check 'upgrade: the old document is gone' (-not (Test-Path (Join-Path $appDir 'docs\OLD_NOTES.md')))
Check 'upgrade: the stale file in the VST3 bundle is gone' (-not (Test-Path (Join-Path $vst3 'Contents\Resources\old.txt')))
Check "upgrade: Apps entry says $version" ((Get-ItemProperty $uninstallKey).DisplayVersion -eq $version)
Check 'upgrade: one Apps entry for Zederia' (@(Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall' | Where-Object { (Get-ItemProperty $_.PSPath).DisplayName -like 'Zederia*' }).Count -eq 1)

# 3. What a full install puts where.
Check 'VST3 module' (Test-Path (Join-Path $vst3 'Contents\x86_64-win\Zederia.vst3'))
Check 'VST3 moduleinfo.json' (Test-Path (Join-Path $vst3 'Contents\Resources\moduleinfo.json'))
Check 'VST3 folder icon (Plugin.ico, desktop.ini hidden and system, folder system)' ((Test-Path (Join-Path $vst3 'Plugin.ico')) -and ((Get-Item -Force (Join-Path $vst3 'desktop.ini')).Attributes -match 'System') -and ((Get-Item $vst3).Attributes -match 'System'))
Check 'CLAP plug-in' (Test-Path $clap)
foreach ($f in @('Zederia.exe', 'zederia-mcp.exe', 'zederia-cli.exe', 'zederia.ico', 'LICENSE.txt', 'NOTICE.txt', 'docs\MANUAL.md', 'docs\RELEASE_NOTES.md', 'docs\AGENT_GUIDE.md', 'docs\images\zederia-128.png', 'unins000.exe')) {
    Check "Program Files\Zederia\$f" (Test-Path (Join-Path $appDir $f))
}
Check 'factory: 184 presets' ((Count (Join-Path $factory 'Presets') '*.zpreset') -eq 184)
Check 'factory: 62 wavetables' ((Count (Join-Path $factory 'Wavetables') '*.wav') -eq 62)
Check 'factory: 21 textures' ((Count (Join-Path $factory 'Textures') '*.wav') -eq 21)
Check 'factory: 24 shapes' ((Count (Join-Path $factory 'Shapes') '*.zshape') -eq 24)
Check 'factory: 16 clips' ((Count (Join-Path $factory 'Clips') '*.mid') -eq 16)
Check 'factory: 12 racks and 8 effect presets' (((Count (Join-Path $factory 'Racks') '*.zrack') -eq 12) -and ((Count (Join-Path $factory 'Racks') '*.zfx') -eq 8))
Check 'factory: the Default preset' (Test-Path (Join-Path $factory 'Presets\Default.zpreset'))
Check 'Start menu entry for the Standalone' (Test-Path $startMenu)
Check 'registry InstallDir for the AGENT page' ((Get-ItemProperty 'HKLM:\SOFTWARE\Zederia').InstallDir -eq $appDir)
$installedVersion = (& (Join-Path $appDir 'zederia-cli.exe') --version | Select-Object -First 1)
Check "installed zederia-cli says $version ($installedVersion)" ($installedVersion -eq "zederia-cli $version")
Check 'installed Standalone carries the version' ((Get-Item (Join-Path $appDir 'Zederia.exe')).VersionInfo.ProductVersion -eq $version)
$sizeMb = [math]::Round(((Get-ChildItem $appDir, $vst3, $factory -Recurse -File -Force | Measure-Object Length -Sum).Sum + (Get-Item $clap).Length) / 1MB, 1)
Say "      installed size: $sizeMb MB"

# 4. A smaller choice of parts: the parts left out go.
Install $setup 'parts' '/COMPONENTS="content,vst3"'
Check 'parts: VST3 kept' (Test-Path (Join-Path $vst3 'Contents\x86_64-win\Zederia.vst3'))
Check 'parts: CLAP removed' (-not (Test-Path $clap))
Check 'parts: Standalone and its Start menu entry removed' (-not (Test-Path (Join-Path $appDir 'Zederia.exe')) -and -not (Test-Path $startMenu))
Check 'parts: agent tools removed' (-not (Test-Path (Join-Path $appDir 'zederia-mcp.exe')) -and -not (Test-Path (Join-Path $appDir 'zederia-cli.exe')))

# 5. Uninstalling.
Install $setup 'full-again'
Uninstall
foreach ($left in @($appDir, $vst3, $clap, (Join-Path $env:ProgramData 'Zederia'), $startMenu, 'HKLM:\SOFTWARE\Zederia', $uninstallKey)) {
    Check "nothing left: $left" (-not (Test-Path $left))
}
Check "your files are as they were: Documents\Zederia ($userBefore)" ((Get-Fingerprint $userDir) -eq $userBefore)
Check "your settings are as they were: %APPDATA%\Zederia ($settingsBefore)" ((Get-Fingerprint $settingsDir) -eq $settingsBefore)

# 6. The current version, installed again for use.
if (-not $NoReinstall) {
    Install $setup 'final'
    Check 'final: installed' ((Test-Path (Join-Path $appDir 'Zederia.exe')) -and (Test-Path $vst3) -and (Test-Path $clap))
}
Say "$failures failure(s)"
exit ([int]($failures -gt 0))
