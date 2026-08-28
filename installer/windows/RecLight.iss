; RecLight Windows installer (Inno Setup script).
;
; Built by the GitHub Actions workflow (.github/workflows/windows-build.yml)
; on a windows-latest runner, after CMake/MSVC produce the VST3 and
; Standalone build artefacts. Not intended to be hand-run on macOS/Linux.
;
; To build locally on a real Windows machine instead:
;   1. cmake -B build -DCMAKE_BUILD_TYPE=Release
;   2. cmake --build build --config Release --target RecLight_VST3 RecLight_Standalone
;   3. iscc installer\windows\RecLight.iss

#define MyAppName "RecLight"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Steinbach Audio"

; Overridable from the command line: iscc /DBuildRoot=..\..\build RecLight.iss
#ifndef BuildRoot
  #define BuildRoot "..\..\build"
#endif

[Setup]
AppId={{B6E2B6B8-6B7B-4B7B-9B7B-6B7B6B7B6B7B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=build
OutputBaseFilename=RecLight-{#MyAppVersion}-Setup
Compression=lzma
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin

[Types]
Name: "full"; Description: "Full installation (VST3 + Standalone app)"
Name: "vst3only"; Description: "VST3 plug-in only"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 Plug-In"; Types: full vst3only custom
Name: "app"; Description: "Standalone App"; Types: full custom

[Files]
Source: "{#BuildRoot}\RecLight_artefacts\Release\VST3\RecLight.vst3\*"; \
    DestDir: "{commoncf64}\VST3\RecLight.vst3"; Components: vst3; \
    Flags: recursesubdirs createallsubdirs ignoreversion

Source: "{#BuildRoot}\RecLight_artefacts\Release\Standalone\RecLight.exe"; \
    DestDir: "{app}"; Components: app; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\RecLight.exe"; Components: app
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
