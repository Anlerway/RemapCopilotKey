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
;   * 开机自启是一个注册在 SCM 里的服务（RemapCopilotKeyAgent，启动类型"自动"）：
;     安装完直接调 RemapCopilotKey.exe --register-service 注册并启动，
;     卸载前调 --unregister-service 停服务并从 SCM 删除。
;     安装 / 卸载都必须是管理员权限，否则 SCM 会直接拒绝（错误 5）。
; ---------------------------------------------------------------------------

#define MyAppName "RemapCopilotKey"
#define MyAppPublisher "Anlerway"
#define MyAppExeName "RemapCopilotKey.exe"
; 与 src\Configurator\ServiceInstaller.cs 里的 ServiceName、以及 C++ 侧
; src\Service\src\ServiceHost.h 里的 kServiceName 必须一字不差
#define MyServiceName "RemapCopilotKeyAgent"
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
; 先注册自启服务（静默，无窗口），再让用户勾选是否现在启动配置程序。
; 这一步失败不会让安装失败（服务注册失败时用户还能从界面里手动点「注册服务」），
; 但会用 StatusMsg 让人看到正在做什么。
Filename: "{app}\{#MyAppExeName}"; Parameters: "--register-service"; StatusMsg: "Registering auto-start service..."; Flags: runhidden waituntilterminated
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; 卸载前清掉服务：停掉服务宿主（它会带走投递出去的钩子进程），再从 SCM 删除。
; 必须在 [Files] 被删除之前跑 —— exe 还在才能调用它。
Filename: "{app}\{#MyAppExeName}"; Parameters: "--unregister-service"; Flags: runhidden waituntilterminated; RunOnceId: "UnregisterAutoStartService"

[Code]
// 安装 / 覆盖 / 卸载前先结束正在运行的进程，否则文件被占用会失败。
//
// 顺序很重要：先停服务，再 taskkill。
// 服务配了"失败后自动重启"（见 ServiceInstaller.SetRestartOnFailure），
// 直接 kill 服务宿主会被 SCM 在 5 秒后重新拉起，正好撞上文件复制。
procedure StopOurService();
var
  ResultCode: Integer;
begin
  Exec('sc.exe', 'stop {#MyServiceName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  // 给它一点时间把投递到会话里的钩子进程收掉
  Sleep(1500);
end;

procedure KillOurProcesses();
var
  ResultCode: Integer;
begin
  StopOurService();
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
