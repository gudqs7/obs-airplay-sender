#ifndef MyAppVersion
  #define MyAppVersion "0.1.6"
#endif

#ifndef MyVersionInfoVersion
  #define MyVersionInfoVersion "0.1.6.0"
#endif

#ifndef MyReleaseDirectory
  #define MyReleaseDirectory "..\release\RelWithDebInfo\obs-airplay-cast"
#endif

#ifndef MyOutputDirectory
  #define MyOutputDirectory "..\release"
#endif

#ifndef MyOutputBaseFilename
  #define MyOutputBaseFilename "obs-airplay-cast-" + MyAppVersion + "-windows-x64-Setup"
#endif

#define MyAppName "OBS AirPlay Sender"
#define MyAppPublisher "gudqs7"
#define MyAppURL "https://github.com/gudqs7/obs-airplay-sender"
#define MyAppExeName "obs64.exe"

[Setup]
AppId={{B636F2CF-9E9E-4EAF-9703-5E44D570C48A}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
VersionInfoVersion={#MyVersionInfoVersion}
DefaultDirName={commonappdata}\obs-studio\plugins\obs-airplay-cast
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
DisableDirPage=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#MyOutputDirectory}
OutputBaseFilename={#MyOutputBaseFilename}
SetupIconFile={#SourcePath}\assets\obs-airplay-sender.ico
UninstallDisplayIcon={app}\bin\64bit\obs-airplay-cast.dll
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UsePreviousAppDir=yes
UsePreviousGroup=no
DirExistsWarning=no
CloseApplications=force
CloseApplicationsFilter=obs64.exe
RestartApplications=no
SetupLogging=yes
AllowNoIcons=yes

[Files]
Source: "{#MyReleaseDirectory}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "*.pdb,*.ilk,*.exp,Uninstall.exe"

[Run]
Filename: "{code:GetObsExecutable}"; WorkingDir: "{code:GetObsWorkingDirectory}"; Description: "Launch OBS Studio"; Flags: nowait postinstall skipifsilent; Check: CanLaunchObs

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
Type: filesandordirs; Name: "{localappdata}\obs-airplay-sender"

[Code]
function GetLegacyInstallDirectory: String;
begin
  Result := ExpandConstant('{commonappdata}\obs-studio\plugins\obs-airplay-cast');
end;

function GetObsExecutable(Value: String): String;
var
  Candidate: String;
begin
  Candidate := ExpandConstant('{pf64}\obs-studio\bin\64bit\obs64.exe');
  if not FileExists(Candidate) then
    Candidate := ExpandConstant('{pf32}\obs-studio\bin\64bit\obs64.exe');
  if FileExists(Candidate) then
    Result := Candidate
  else
    Result := '';
end;

function GetObsWorkingDirectory(Value: String): String;
begin
  Result := ExtractFileDir(GetObsExecutable(''));
end;

function CanLaunchObs: Boolean;
begin
  Result := GetObsExecutable('') <> '';
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  LegacyDirectory: String;
  LegacyDll: String;
  LegacyArguments: String;
  LegacyUninstaller: String;
  ResultCode: Integer;
  Attempt: Integer;
begin
  Result := '';
  LegacyDirectory := GetLegacyInstallDirectory;
  LegacyDll := AddBackslash(LegacyDirectory) + 'bin\64bit\obs-airplay-cast.dll';

  if not FileExists(LegacyDll) then
    Exit;

  LegacyUninstaller := AddBackslash(LegacyDirectory) + 'unins000.exe';
  if FileExists(LegacyUninstaller) then
    LegacyArguments := '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART'
  else
  begin
    LegacyUninstaller := AddBackslash(LegacyDirectory) + 'Uninstall.exe';
    if FileExists(LegacyUninstaller) then
      LegacyArguments := '--uninstall --silent'
    else
    begin
      { Manually copied or incomplete legacy installs have no uninstaller.
        The regular [Files] step can safely overwrite their plugin payload. }
      Exit;
    end;
  end;

  if not Exec(
    LegacyUninstaller,
    LegacyArguments,
    '',
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode) then
  begin
    Result :=
      '无法启动旧版卸载程序。请关闭 OBS 后重试，或手动运行：' + #13#10 +
      LegacyUninstaller;
    Exit;
  end;

  if ResultCode <> 0 then
  begin
    Result :=
      '旧版插件卸载失败，退出码：' + IntToStr(ResultCode) + #13#10 +
      '请关闭 OBS 后重试，或手动运行旧版卸载程序。';
    Exit;
  end;

  for Attempt := 1 to 120 do
  begin
    if not FileExists(LegacyDll) then
      Break;
    Sleep(250);
  end;

  if FileExists(LegacyDll) then
    Result :=
      '旧版系统级插件仍在运行或无法删除。' + #13#10 +
      '请关闭 OBS 后重试。';
end;
