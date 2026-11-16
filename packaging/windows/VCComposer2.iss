; VC Composer - Windows installer (Inno Setup 6, preinstalled on GitHub's
; Windows runners). Built by .github/workflows/preview.yml:
;   iscc /DAppVersion=0.1.0 /DSourceDir=<staged files> /DOutputDir=<dist> /DPreview=1 VCComposer2.iss
; The staged folder holds VC Composer.exe, the plug-in scanner and host
; helpers beside it, the LGPL FFmpeg DLLs and the licence notices.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef SourceDir
  #define SourceDir "stage"
#endif
#ifndef OutputDir
  #define OutputDir "dist"
#endif
#ifdef Preview
  #define AppTitle "VC Composer Preview"
  #define FileTag "Preview-"
#else
  #define AppTitle "VC Composer"
  #define FileTag ""
#endif

[Setup]
AppId={{6C0E7B1A-3E5F-4E0B-9C47-2B1D8A9C3F21}
AppName={#AppTitle}
AppVersion={#AppVersion}
AppPublisher=VC Software
AppPublisherURL=https://vc-software.com
AppSupportURL=https://vc-software.com/support
DefaultDirName={autopf}\VC Composer
DefaultGroupName=VC Composer
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=VC-Composer-{#FileTag}{#AppVersion}-Windows-Setup
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
WizardStyle=modern
UninstallDisplayIcon={app}\VC Composer.exe
ChangesAssociations=yes
LicenseFile={#SourceDir}\licenses\EULA.txt

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppTitle}"; Filename: "{app}\VC Composer.exe"
Name: "{group}\Uninstall {#AppTitle}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppTitle}"; Filename: "{app}\VC Composer.exe"; Tasks: desktopicon

[Registry]
; .vcc2 projects open in VC Composer.
Root: HKA; Subkey: "Software\Classes\.vcc2"; ValueType: string; ValueName: ""; ValueData: "VCComposer2.Project"; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\VCComposer2.Project"; ValueType: string; ValueName: ""; ValueData: "VC Composer Project"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\VCComposer2.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\VC Composer.exe,0"
Root: HKA; Subkey: "Software\Classes\VCComposer2.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\VC Composer.exe"" ""%1"""

[Run]
Filename: "{app}\VC Composer.exe"; Description: "{cm:LaunchProgram,{#AppTitle}}"; Flags: nowait postinstall skipifsilent
