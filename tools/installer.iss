; ---------------------------------------------------------------------------
; RemapCopilotKey 安装程序（Inno Setup）
;
; 由 tools\package.ps1 调用，参数全部通过 /D 传入：
;   ISCC.exe installer.iss /DAppSource=<发布目录> /DAppVersion=1.0.0 \
;                          /DOutputName=RemapCopilotKey-v1.0.0-x64-setup \
;                          /DOutputDir=<产物目录>
;
; 注意：
;   * PrivilegesRequired=admin —— 服务程序与配置程序都要求管理员权限。
;   * 安装阶段不检测任何运行环境：.NET Framework 4.8 是 Win10 1903+ / Win11
;     系统自带的组件，不需要装。
;   * 卸载时**不**自动删除开机自启的计划任务：那是用户自己注册的，
;     应该用配置程序里的「结束并清除服务」来清，卸载器不该擅自动系统任务。
; ---------------------------------------------------------------------------

#define MyAppName "RemapCopilotKey"
#define MyAppPublisher "Anlerway"
#define MyAppExeName "RemapCopilotKey.exe"
#define MyAppUrl "https://github.com/Anlerway/RemapCopilotKey"

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef AppSource
  #define AppSource "."
#endif
#ifndef OutputName
  #define OutputName "RemapCopilotKey-setup"
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
; AppId 一经发布就不能改，否则会变成"另一个程序"而无法覆盖安装
AppId={{7C4E1A92-5D38-4B6F-A2E1-9F0B3C7D5A84}
AppName={#MyAppName}
AppVersion={#AppVersion}
AppVerName={#MyAppName} {#AppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppUrl}
AppSupportURL={#MyAppUrl}
DefaultDirName={autopf}\RemapCopilotKey
DefaultGroupName=RemapCopilotKey
DisableProgramGroupPage=yes
; 两个 exe 都需要管理员权限，安装程序跟着提权
PrivilegesRequired=admin
CloseApplications=yes
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}
VersionInfoVersion={#AppVersion}
VersionInfoProductVersion={#AppVersion}
VersionInfoProductName={#MyAppName}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} {#AppVersion} Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\src\Configurator\Assets\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; 只用 Inno Setup 自带的默认语言（英文向导）。
; 中文向导需要额外的 Languages\ChineseSimplified.isl，而它并不随 Inno Setup 一起分发，
; 为了一个可选安装程序去 vendor 一份第三方翻译文件不划算 —— 绿色包才是主要交付方式。
[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

; 覆盖安装前清空旧目录，避免上一版残留的文件混在新版里
[InstallDelete]
Type: filesandordirs; Name: "{app}\*"

[Files]
Source: "{#AppSource}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[Code]
// 安装 / 覆盖 / 卸载前先结束正在运行的进程，否则文件被占用会失败。
// 服务程序也要一起结束，它同样锁着 RemapCopilotKeyService.exe。
procedure KillOurProcesses();
var
  ResultCode: Integer;
begin
  Exec('taskkill.exe', '/IM RemapCopilotKey.exe /T /F', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec('taskkill.exe', '/IM RemapCopilotKeyService.exe /T /F', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  KillOurProcesses();
  Result := '';
end;

function PrepareToUninstall(var Msg: String): Boolean;
begin
  KillOurProcesses();
  Result := True;
end;
