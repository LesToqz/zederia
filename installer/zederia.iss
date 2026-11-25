; The Zederia installer (Inno Setup 7). scripts/package.ps1 stages the built files and the factory
; content in build\package\stage and compiles this script with the version of the build:
;   ISCC /DAppVersion=1.0.0-rc1 /DNumericVersion=1.0.0.0 /DStage=<stage> /DOutputDir=<folder> installer\zederia.iss
;
; What goes where (administrator rights once, for every user of the computer):
;   VST3        C:\Program Files\Common Files\VST3\Zederia.vst3
;   CLAP        C:\Program Files\Common Files\CLAP\Zederia.clap
;   Standalone and tools   C:\Program Files\Zederia
;   factory content        %PROGRAMDATA%\Zederia\Factory
; The uninstaller removes all of it and keeps what belongs to the user: Documents\Zederia (presets,
; wavetables, renders), the settings and the agent permissions.

#ifndef AppVersion
  #error AppVersion is not defined: build the installer with scripts/package.ps1
#endif
#ifndef NumericVersion
  #error NumericVersion is not defined: build the installer with scripts/package.ps1
#endif
#ifndef Stage
  #error Stage is not defined: build the installer with scripts/package.ps1
#endif
#ifndef OutputDir
  #define OutputDir "..\build\package"
#endif

[Setup]
; Never change AppId: an upgrade finds the installed version by it.
AppId={{9539CA56-225D-4CE5-A412-00AAEA93F8E2}
AppName=Zederia
AppVersion={#AppVersion}
AppVerName=Zederia {#AppVersion}
AppPublisher=LesToqz
AppCopyright=Copyright (C) 2026 LesToqz. Free software under the GNU AGPL v3 or later.
AppComments=Wavetable synthesizer: VST3 and CLAP plug-ins, Standalone application and agent tools
DefaultDirName={autopf}\Zederia
; The plug-in folders are fixed by their formats and the tools folder is where the plug-in looks
; for them, so there is no folder to choose.
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=ZederiaSetup-{#AppVersion}
SetupIconFile=..\plugin\brand\zederia.ico
UninstallDisplayIcon={app}\zederia.ico
UninstallDisplayName=Zederia {#AppVersion}
LicenseFile=..\LICENSE
WizardStyle=modern dynamic
DisableWelcomePage=no
WizardImageFile=images\wizard-side-202x386.png,images\wizard-side-269x515.png,images\wizard-side-336x643.png,images\wizard-side-403x772.png,images\wizard-side-430x824.png,images\wizard-side-498x953.png,images\wizard-side-534x1022.png
WizardSmallImageFile=images\wizard-small-58.png,images\wizard-small-77.png,images\wizard-small-97.png,images\wizard-small-116.png,images\wizard-small-124.png,images\wizard-small-143.png,images\wizard-small-159.png
Compression=lzma2/max
SolidCompression=yes
; Programs that keep installed files open (a host with the plug-in loaded, the Standalone, an agent
; client's zederia-mcp) are offered to be closed before files are replaced.
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#NumericVersion}
VersionInfoProductVersion={#NumericVersion}
VersionInfoTextVersion={#AppVersion}
VersionInfoProductTextVersion={#AppVersion}
VersionInfoProductName=Zederia
VersionInfoDescription=Zederia {#AppVersion} Setup
VersionInfoCompany=LesToqz
VersionInfoCopyright=Copyright (C) 2026 LesToqz

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
WelcomeLabel2=This will install [name/ver] on your computer.%n%nThe VST3 and CLAP plug-ins go to the standard Common Files folders, where hosts such as FL Studio find them without extra search folders. The Standalone application and the agent tools go to Program Files, and the factory content is shared by every user of this computer.%n%nYour own presets, wavetables and settings in Documents\Zederia stay when you upgrade or uninstall.%n%nClose any host that has Zederia loaded before you continue.

[Types]
Name: "full"; Description: "Everything"
Name: "custom"; Description: "Choose the parts"; Flags: iscustom

[Components]
Name: "content"; Description: "Factory content (presets, wavetables, textures, shapes, clips, effect racks)"; Types: full custom; Flags: fixed
Name: "vst3"; Description: "VST3 plug-in"; Types: full
Name: "clap"; Description: "CLAP plug-in"; Types: full
Name: "standalone"; Description: "Standalone application (with a Start menu entry)"; Types: full
Name: "agent"; Description: "Agent tools: zederia-mcp (the agent bridge) and zederia-cli"; Types: full

[InstallDelete]
; Everything an earlier version installed goes first, so an upgrade leaves no stale files behind
; and a part that is no longer chosen is removed. [Files] then installs the chosen parts afresh.
Type: filesandordirs; Name: "{commoncf64}\VST3\Zederia.vst3"
Type: files; Name: "{commoncf64}\CLAP\Zederia.clap"
Type: filesandordirs; Name: "{commonappdata}\Zederia\Factory"
Type: files; Name: "{app}\Zederia.exe"
Type: files; Name: "{app}\zederia-mcp.exe"
Type: files; Name: "{app}\zederia-cli.exe"
Type: filesandordirs; Name: "{app}\docs"
Type: files; Name: "{autoprograms}\Zederia.lnk"

[Dirs]
; The VST3 folder shows the logo in Explorer through its desktop.ini, which needs the folder's
; system attribute.
Name: "{commoncf64}\VST3\Zederia.vst3"; Attribs: system; Components: vst3

[Files]
Source: "{#Stage}\VST3\Zederia.vst3\*"; DestDir: "{commoncf64}\VST3\Zederia.vst3"; Excludes: "desktop.ini"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Stage}\VST3\Zederia.vst3\desktop.ini"; DestDir: "{commoncf64}\VST3\Zederia.vst3"; Attribs: hidden system; Components: vst3; Flags: ignoreversion
Source: "{#Stage}\CLAP\Zederia.clap"; DestDir: "{commoncf64}\CLAP"; Components: clap; Flags: ignoreversion
Source: "{#Stage}\Zederia.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "{#Stage}\zederia-mcp.exe"; DestDir: "{app}"; Components: agent; Flags: ignoreversion
Source: "{#Stage}\zederia-cli.exe"; DestDir: "{app}"; Components: agent; Flags: ignoreversion
Source: "{#Stage}\Factory\*"; DestDir: "{commonappdata}\Zederia\Factory"; Components: content; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Stage}\zederia.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\NOTICE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Zederia"; Filename: "{app}\Zederia.exe"; WorkingDir: "{app}"; Comment: "Zederia wavetable synthesizer"; Components: standalone

[Registry]
; The plug-in's AGENT page finds zederia-mcp through InstallDir.
Root: HKLM; Subkey: "Software\Zederia"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Zederia"; ValueType: string; ValueName: "InstallDir"; ValueData: "{app}"
Root: HKLM; Subkey: "Software\Zederia"; ValueType: string; ValueName: "Version"; ValueData: "{#AppVersion}"
Root: HKLM; Subkey: "Software\Zederia"; ValueType: string; ValueName: "FactoryDir"; ValueData: "{commonappdata}\Zederia\Factory"

[Run]
Filename: "{app}\Zederia.exe"; Description: "Start Zederia"; Components: standalone; Flags: postinstall nowait skipifsilent unchecked

[UninstallDelete]
; Only what the installer made: the factory folder (whole, also files a later version added) and
; the folders it created. Documents\Zederia, %APPDATA%\Zederia and %LOCALAPPDATA%\Zederia stay.
Type: filesandordirs; Name: "{commonappdata}\Zederia\Factory"
Type: dirifempty; Name: "{commonappdata}\Zederia"
Type: dirifempty; Name: "{app}\docs"
Type: dirifempty; Name: "{app}"

[Code]
// A development install (scripts/install-dev.ps1) puts a second copy of the plug-ins in the
// per-user folders; hosts that scan those would list Zederia twice.
function DevelopmentInstallFound(): Boolean;
begin
  Result := DirExists(ExpandConstant('{localappdata}\Programs\Common\VST3\Zederia.vst3')) or
            FileExists(ExpandConstant('{localappdata}\Programs\Common\CLAP\Zederia.clap'));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and DevelopmentInstallFound() then
    SuppressibleMsgBox('A development copy of Zederia is also installed in ' +
      ExpandConstant('{localappdata}\Programs\Common') + '. Hosts that scan that folder list Zederia twice. ' +
      'Remove it with scripts\install-dev.ps1 -Uninstall in the source folder.', mbInformation, MB_OK, IDOK);
end;
