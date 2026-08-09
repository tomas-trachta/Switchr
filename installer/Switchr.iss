; Switchr installer -- Inno Setup 6.
; Per-user install, no UAC (Switchr itself always runs asInvoker). Adds
; Start Menu / desktop shortcuts and an optional "launch at startup" entry.
; Drives the build via installer\build.ps1, which stages the exe into stage\.

#ifndef AppVersion
#define AppVersion "1.0.0"
#endif

[Setup]
; Stable per-product GUID -- do NOT reuse the .vcxproj GUID. Changing this
; would orphan previous installs in Add/Remove Programs.
AppId={{6F1C2E9A-4B7D-4A2C-8E15-9D3B7A2C5F60}
AppName=Switchr
AppVersion={#AppVersion}
AppPublisher=Tomas Trachta
LicenseFile=..\LICENSE
DefaultDirName={localappdata}\Programs\Switchr
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=output
OutputBaseFilename=Switchr-setup-x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Switchr
UninstallDisplayIcon={app}\Switchr.exe
SetupIconFile=Switchr.ico

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop icon"; GroupDescription: "Additional icons:"; Flags: unchecked
Name: "startup";     Description: "Launch Switchr when you sign in"; GroupDescription: "Integration:"; Flags: unchecked

[Files]
Source: "stage\Switchr.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{userprograms}\Switchr"; Filename: "{app}\Switchr.exe"
Name: "{userdesktop}\Switchr"; Filename: "{app}\Switchr.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
  ValueType: string; ValueName: "Switchr"; ValueData: """{app}\Switchr.exe"""; \
  Tasks: startup; Flags: uninsdeletevalue

[Run]
Filename: "{app}\Switchr.exe"; Description: "Launch Switchr"; Flags: nowait postinstall skipifsilent
