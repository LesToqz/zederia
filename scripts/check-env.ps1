# Re-runnable environment check. Exits non-zero if a required tool is missing.
$ErrorActionPreference = 'Continue'
$fail = $false
function Ver($s) { if ($s -match '(\d+)\.(\d+)(\.(\d+))?') { [version]("$($Matches[1]).$($Matches[2]).$(if($Matches[4]){$Matches[4]}else{0})") } }
function Req($name, $ok, $text) { if ($ok) { "{0,-14} {1}" -f $name, $text } else { "{0,-14} MISSING or too old ({1})" -f $name, $text; $script:fail = $true } }

$os = Get-CimInstance Win32_OperatingSystem; $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
Add-Type -MemberDefinition '[DllImport("kernel32.dll")] public static extern bool IsProcessorFeaturePresent(int f);' -Name K -Namespace W -ErrorAction SilentlyContinue
$free = [math]::Round((Get-PSDrive C).Free / 1GB, 1)
"OS            $($os.Caption) $($os.Version)"
"CPU           $($cpu.Name.Trim())  AVX2=$([W.K]::IsProcessorFeaturePresent(40))"
"RAM GB        $([math]::Round($os.TotalVisibleMemorySize/1MB,1))"
Req 'Disk GB free' ($free -ge 20) "$free (need 20)"
"Audio         $((Get-CimInstance Win32_SoundDevice | Select-Object -First 1).Name)"

$g = Get-Command git -ErrorAction SilentlyContinue
Req 'git' ($g -and (Ver (git --version)) -ge [version]'2.40.0') $(if ($g) { git --version } else { 'not found' })
$c = Get-Command cmake -ErrorAction SilentlyContinue
Req 'cmake' ($c -and (Ver ((cmake --version)[0])) -ge [version]'3.25.0') $(if ($c) { (cmake --version)[0] } else { 'not found' })
$n = Get-Command ninja -ErrorAction SilentlyContinue
Req 'ninja' ([bool]$n) $(if ($n) { "ninja $(ninja --version)" } else { 'not found' })

$vw = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vw) { & $vw -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } 
$ok = $false; $text = 'no VC tools via vswhere'
if ($vs) {
    $t = Join-Path $env:TEMP 'zed_envcheck'; New-Item -ItemType Directory -Force $t | Out-Null
    Set-Content "$t\t.cpp" '#include <format>' , '#include <iostream>', 'int main(){ std::cout << std::format("ok {}", 20); }'
    $out = cmd /c "call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && cd /d $t && cl /nologo /std:c++20 /EHsc t.cpp >nul && .\t.exe" 2>$null
    $ok = ($out -match 'ok 20'); $text = "$vs; C++20 compile+run " + $(if ($ok) { 'OK' } else { 'FAILED' })
}
Req 'msvc (v143+)' $ok $text
$sdk = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Include" -ErrorAction SilentlyContinue | Select-Object -Last 1
Req 'windows sdk' ([bool]$sdk) $(if ($sdk) { $sdk.Name } else { 'not found' })

"--- optional ---"
foreach ($t in 'clang-cl', 'node', 'iscc', 'claude', 'codex', 'agy') {
    $x = Get-Command $t -ErrorAction SilentlyContinue; "{0,-14} {1}" -f $t, $(if ($x) { $x.Source } else { 'not found' })
}
if ($fail) { "`nRESULT: FAIL"; exit 1 } else { "`nRESULT: OK"; exit 0 }
