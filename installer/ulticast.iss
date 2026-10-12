; Ulticast installer (Inno Setup 6).
; The build copies the deployable files into dist/ first; this packages them.
;   ISCC.exe /DAppVersion=0.4.0 installer\ulticast.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{8F2B0C7A-9C41-4C0E-9A77-5F6C2A1B7D31}
AppName=Ulticast
AppVersion={#AppVersion}
AppPublisher=modnite
AppPublisherURL=https://github.com/modnite/ulticast
AppSupportURL=https://github.com/modnite/ulticast/issues
DefaultDirName={autopf}\Ulticast
DefaultGroupName=Ulticast
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\ulticast-ui.exe
OutputDir=Output
OutputBaseFilename=Ulticast-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"

[Files]
Source: "..\dist\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\Ulticast"; Filename: "{app}\ulticast-ui.exe"
Name: "{group}\Ulticast Diagnostics"; Filename: "{app}\ulticast.exe"; Parameters: "--diagnostics"
Name: "{group}\Ulticast Logs"; Filename: "{app}\ulticast.exe"; Parameters: "--open-logs"
Name: "{autodesktop}\Ulticast"; Filename: "{app}\ulticast-ui.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\ulticast-ui.exe"; Description: "Launch Ulticast"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
