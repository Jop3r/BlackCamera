; BlackCamera installer (Inno Setup 6)
;
; Build it with tools\make-installer.ps1, which first produces:
;   dist\BlackCamera\            Release build + DLLs + model  (cmake --install)
;                                 includes BlackCameraVCam.dll, the Windows 11 virtual camera source
;   build\redist\vc_redist.x64.exe  Microsoft Visual C++ Redistributable
; Output: dist\BlackCamera_Setup_v<version>.exe

#define MyAppName "BlackCamera"
#ifndef MyAppVersion
  #define MyAppVersion "1.2.0"
#endif
#define MyAppPublisher "Jop3r"
#define MyAppURL "https://github.com/Jop3r/BlackCamera"
#define MyAppExeName "BlackCamera.exe"

[Setup]
AppId={{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=LICENSE
OutputDir=dist
OutputBaseFilename=BlackCamera_Setup_v{#MyAppVersion}
#if FileExists("BlackCamera\app.ico")
SetupIconFile=BlackCamera\app.ico
#endif
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Program Files + VC++ runtime need elevation
PrivilegesRequired=admin

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "dist\BlackCamera\*"; DestDir: "{app}"; Excludes: "BlackCameraVCam.dll"; Flags: ignoreversion recursesubdirs createallsubdirs
; Media source of the built-in "BlackCamera" virtual camera (Windows 11 build 22000+ only).
; regserver runs DllRegisterServer (HKLM, needed by the Frame Server service) and unregisters on uninstall.
; The Frame Server may hold the DLL while an app uses the camera, hence the restart flags.
Source: "dist\BlackCamera\BlackCameraVCam.dll"; DestDir: "{app}"; MinVersion: 10.0.22000; Flags: ignoreversion regserver restartreplace uninsrestartdelete
Source: "build\redist\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
; The redistributable skips itself when the same or a newer runtime is already installed
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Microsoft Visual C++ Runtime..."; Flags: waituntilterminated
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; User settings (%APPDATA%\BlackCamera) and logs (%LOCALAPPDATA%\BlackCamera) are kept
Type: dirifempty; Name: "{app}"
