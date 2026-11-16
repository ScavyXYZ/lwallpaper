#ifndef AppVersion
  #define AppVersion "1.3"
#endif
#ifndef BuildDir
  #define BuildDir "..\build\bin\Release"
#endif
#ifndef RedistPath
  #define RedistPath "redist\VC_redist.x64.exe"
#endif
#ifdef RoseEdition
  #define AppName "LWallpaper Rose"
  #define AppKey "LWallpaperRose"
  #define AppGuid "{{5C806EA3-DB26-4454-826E-61338667804C}"
  #define OutputName "lwallpaper-vgui-" + AppVersion + "-windows-x64-setup"
#else
  #define AppName "LWallpaper"
  #define AppKey "LWallpaper"
  #define AppGuid "{{E20BF12D-15C8-4A2D-80E5-D4047FA09EF3}"
  #define OutputName "lwallpaper-" + AppVersion + "-windows-x64-setup"
#endif

[Setup]
AppId={#AppGuid}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=ScavyXYZ
AppPublisherURL=https://github.com/ScavyXYZ/lwallpaper
DefaultDirName={localappdata}\Programs\{#AppKey}
DefaultGroupName={#AppName}
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\build\installer
OutputBaseFilename={#OutputName}
SetupIconFile=..\resources\icons\lwallpaper.ico
UninstallDisplayIcon={app}\lwallpaper.exe
Compression=lzma2/fast
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
Uninstallable=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "ukrainian"; MessagesFile: "compiler:Languages\Ukrainian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked
Name: "autostart"; Description: "Start wallpaper automatically when I sign in"; Flags: unchecked

[Files]
Source: "{#RedistPath}"; Flags: dontcopy
Source: "{#BuildDir}\*"; DestDir: "{app}"; Excludes: "*tests*.exe,*.pdb"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\third_party\ffmpeg\LICENSE"; DestDir: "{app}\licenses"; DestName: "FFmpeg.txt"; Flags: ignoreversion
Source: "..\third_party\SDL3\LICENSE.txt"; DestDir: "{app}\licenses"; DestName: "SDL3.txt"; Flags: ignoreversion

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#AppKey}"; ValueData: """{app}\lwallpaper.exe"" --autostart"; Tasks: autostart; Flags: uninsdeletevalue

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\lwallpaper.exe"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\lwallpaper.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\lwallpaper.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[Code]
var
  RuntimeRestart: Boolean;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Installed: Cardinal;
  Code: Integer;
begin
  Result := '';
  if RegQueryDWordValue(HKLM64, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Installed) then
    if Installed = 1 then exit;
  ExtractTemporaryFile('VC_redist.x64.exe');
  if not ShellExec('runas', ExpandConstant('{tmp}\VC_redist.x64.exe'),
    '/install /quiet /norestart', '', SW_HIDE, ewWaitUntilTerminated, Code) then begin
    Result := 'Microsoft Visual C++ x64 runtime installation was not started. Please allow the administrator prompt.';
    exit;
  end;
  if (Code <> 0) and (Code <> 3010) and (Code <> 1638) then
    Result := Format('Microsoft Visual C++ runtime installation failed (code %d).', [Code]);
  RuntimeRestart := Code = 3010;
end;

function NeedRestart(): Boolean;
begin
  Result := RuntimeRestart;
end;
