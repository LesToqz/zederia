# Rebuilds the icons and the installer's images from the logo, plugin/brand/zederia-mark.svg:
#   plugin/brand/zederia.ico          16 to 256 px, for the Standalone, the plug-ins, the tools and the installer
#   plugin/brand/png/zederia-*.png    the same sizes as PNG files
#   installer/images/wizard-*.png     the installer's side and small images at 100 to 250 % scaling
# It fails when the gap between the two slabs closes at 16 or 32 px. Run it after changing the
# logo, then build again (the programs embed the icon) and commit the files it writes.
#   scripts/make-icons.ps1
#   scripts/make-icons.ps1 -Config Debug
param([ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Config = 'Release')
. "$PSScriptRoot\_env.ps1"
$cli = Join-Path $RepoRoot "build\$($Config.ToLower())\cli\zederia-cli.exe"
if (-not (Test-Path $cli)) { throw "$cli not found; run scripts/build.ps1 -Config $Config first" }
Invoke-Checked $cli brand --svg plugin/brand/zederia-mark.svg --out plugin/brand --installer installer/images
