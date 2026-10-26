#Requires -Version 5.1
<#
.SYNOPSIS
    RemapCopilotKey 生产版打包：绿色包 + 可选的安装程序。

.DESCRIPTION
    流程：

        1. 编译 C++ 服务程序与 C# 配置程序（MSBuild）
        2. 组装发布目录：只放真正要发给用户的文件
        3. 写好"使用说明.txt"
        4. 清除 Mark-of-the-Web（网络标记）
        5. 压成 zip
        6. 生成 SHA256 校验值
        7. 如果装了 Inno Setup，再打一个安装程序（可选，失败不影响绿色包）

    为什么只有一个绿色包：
        配置程序是 .NET Framework 4.8 的 WPF 应用，而 net48 是 Windows 10 1903+
        / Windows 11 系统自带的组件。所以不需要再像 .NET 8 时代那样区分
        "自带运行时版（~70 MB）"和"框架依赖版"，目标机器什么都不用装。

    产物（默认在 <仓库>\artifacts\）：
        release\RemapCopilotKey\                          解压即用的完整目录
        release\RemapCopilotKey-v<版本>-x64.zip           绿色包
        release\SHA256SUMS.txt                            校验值
        installer\RemapCopilotKey-v<版本>-x64-setup.exe   安装程序（可选）

.PARAMETER Version
    版本号，写进产物文件名。

.PARAMETER SkipBuild
    跳过编译，直接用 build\out 里已有的产物。

.PARAMETER NoZip
    只产出目录，不压缩、不生成校验值。

.PARAMETER NoInstaller
    跳过 Inno Setup 安装程序。

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\package.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\package.ps1 -SkipBuild -NoInstaller
#>
[CmdletBinding()]
param(
    [string] $Version       = '1.0.0',
    [string] $Configuration = 'Release',
    [string] $ArtifactsRoot = '',
    [string] $Iscc          = '',

    [switch] $SkipBuild,
    [switch] $NoZip,
    [switch] $NoInstaller
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $ArtifactsRoot) { $ArtifactsRoot = Join-Path $RepoRoot 'artifacts' }

$ReleaseRoot   = Join-Path $ArtifactsRoot 'release'
$InstallerRoot = Join-Path $ArtifactsRoot 'installer'
$OutRoot       = Join-Path $RepoRoot 'build\out'
$ServiceExe    = Join-Path $OutRoot 'Service\RemapCopilotKeyService.exe'
$ConfigDir     = Join-Path $OutRoot 'Configurator'
$ConfigExe     = Join-Path $ConfigDir 'RemapCopilotKey.exe'
$BuildScript   = Join-Path $RepoRoot 'build.ps1'

$PackageName = "RemapCopilotKey-v$Version-x64"
$DistDir     = Join-Path $ReleaseRoot 'RemapCopilotKey'
$ZipPath     = Join-Path $ReleaseRoot "$PackageName.zip"
$SumsPath    = Join-Path $ReleaseRoot 'SHA256SUMS.txt'

function Write-Step { param([string]$m) Write-Host "`n=== $m ===" -ForegroundColor Cyan }
function Write-Ok   { param([string]$m) Write-Host "[OK] $m" -ForegroundColor Green }
function Write-Info { param([string]$m) Write-Host "[i]  $m" -ForegroundColor DarkGray }
function Write-Warn { param([string]$m) Write-Host "[!]  $m" -ForegroundColor Yellow }

Write-Step "RemapCopilotKey $Version 生产版打包"
Write-Info "仓库: $RepoRoot"
Write-Info "产物: $ArtifactsRoot"

# ---------------------------------------------------------------------------
# 0) 请正在运行的实例退出，并重建产物目录
#
# 正在运行的 exe 会被 Windows 锁住，删不掉目录 —— 不做这一步，
# 第二次打包就会莫名其妙地失败。
# ---------------------------------------------------------------------------
Write-Step '清理旧产物'

foreach ($name in @('RemapCopilotKey', 'RemapCopilotKeyService')) {
    Get-Process -Name $name -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Info "结束正在运行的 $name (pid=$($_.Id))"
        try { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue } catch { }
    }
}
Start-Sleep -Milliseconds 500

function Remove-DirectoryWithRetry {
    param([string]$Path, [int]$Attempts = 5)

    for ($i = 1; $i -le $Attempts; $i++) {
        if (-not (Test-Path -LiteralPath $Path)) { return }
        try {
            Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
            return
        }
        catch {
            if ($i -eq $Attempts) {
                Write-Host ("[x]  无法删除 {0}：{1}" -f $Path, $_.Exception.Message) -ForegroundColor Red
                Write-Host '     请先关闭正在运行的 RemapCopilotKey.exe 与 RemapCopilotKeyService.exe 再重试。' -ForegroundColor Yellow
                throw
            }
            Start-Sleep -Milliseconds 400
        }
    }
}

Remove-DirectoryWithRetry -Path $ReleaseRoot
Remove-DirectoryWithRetry -Path $InstallerRoot
New-Item -ItemType Directory -Force -Path $ReleaseRoot | Out-Null

# ---------------------------------------------------------------------------
# 1) 编译
# ---------------------------------------------------------------------------
if (-not $SkipBuild) {
    Write-Step '编译（C++ 服务程序 + C# 配置程序）'

    foreach ($task in @('service', 'config')) {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $BuildScript $task
        if ($LASTEXITCODE -ne 0) { throw "build.ps1 $task 失败（退出码 $LASTEXITCODE）" }
    }
}

foreach ($required in @($ServiceExe, $ConfigExe)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "缺少产物：$required`n请先运行 build.ps1。"
    }
}

# ---------------------------------------------------------------------------
# 2) 组装发布目录
#
# 只放运行需要的文件：编译中间产物（.deps.json / .runtimeconfig.json / .pdb）
# 一概不打包 —— 它们只会在用户解压后造成困惑。
# 服务程序与配置程序**必须放在同一个目录**：两者共用同目录下的 config.json。
# ---------------------------------------------------------------------------
Write-Step "组装 -> $DistDir"
New-Item -ItemType Directory -Force -Path $DistDir | Out-Null

# 配置程序：exe + app.config + 依赖的库（目前是 .NET Framework 自带的那些，没有额外 dll）。
#
# 这里用"从产物目录整体拷贝、再排除不需要的"方式，而不是逐个文件点名 ——
# 点名的话，以后配置程序多一个依赖就会漏拷，而且是运行到那一步才发现。
# 原则：**只排除确定不要的**。
foreach ($file in Get-ChildItem -LiteralPath $ConfigDir -Recurse -File) {
    $relative = $file.FullName.Substring($ConfigDir.Length + 1)

    # 符号文件不发布
    if ($file.Extension -eq '.pdb') { continue }
    # 运行期产物（服务日志、启动追踪、配置）不属于程序本身。
    # 启动追踪尤其要注意：在 build\out 里跑过一次服务程序就会留下它。
    if ($file.Name -eq 'RemapCopilotKeyService.log') { continue }
    if ($file.Name -eq 'RemapCopilotKey.boot.log') { continue }
    if ($file.Name -eq 'config.json') { continue }

    $target = Join-Path $DistDir $relative
    $targetDir = Split-Path -Parent $target
    if (-not (Test-Path -LiteralPath $targetDir)) {
        New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
    }
    Copy-Item -LiteralPath $file.FullName -Destination $target -Force
}

# C++ 服务程序
Copy-Item -LiteralPath $ServiceExe -Destination $DistDir -Force

# 使用说明
$readme = @"
RemapCopilotKey v$Version
========================================

第一次使用
----------
1. 运行 RemapCopilotKey.exe（配置程序，需要管理员权限，会弹一次 UAC）。
2. 选择 Copilot 键要映射成的目标 —— 改完会自动保存，没有「保存」按钮，
   底部那行会显示「已自动保存 HH:mm:ss」。
3. 点「注册服务」—— 注册为 Windows 服务（RemapCopilotKeyAgent，LocalSystem、
   启动类型「自动」）并立刻启动；以后开机自动运行，不会再弹 UAC。

   不想开机自启也可以只点「启动服务」手动运行（每次会弹一次 UAC）。
   用安装包（package.cmd -Installer）装的话，第 3 步已经自动做过了。
   要卸载服务就点「结束并清除服务」（停止 + 从 SCM 删除）。
   想整体回到出厂设置点底部的「恢复默认配置」。

开机自启是怎么工作的
--------------------
RemapCopilotKeyService.exe 有两种跑法：

  * 钩子模式（默认 / --run）：在你的交互会话里跑，高完整性(0x3000)，
    装低级键盘钩子；开机自启时由服务宿主投递进来。
  * 服务模式（--service）：由 SCM 拉起的 Windows 服务宿主，LocalSystem、
    常驻 Session 0，只负责把钩子进程投递进你的交互会话
    （WTSQueryUserToken → CreateProcessAsUser），自己不装钩子
    （Session 0 收不到交互桌面的输入）。

所以开机自启的链条是：开机 → SCM 拉起服务宿主 → 你登录 → 钩子进程被投递进
你的会话。全程不经过 UAC。

判断有没有生效：看程序目录下的 RemapCopilotKey.boot.log（不受 logLevel 影响）。
里面会有两行「运行环境」——

   运行环境: 账户=NT AUTHORITY\SYSTEM 会话=0 ... 模式=service   <- 服务宿主，正常
   运行环境: 账户=... 会话=1 ... 完整性=高(0x3000) 提权=是        <- 钩子进程，看这行

钩子进程那行必须是 会话 不为 0 且 完整性=高(0x3000)。

卸载与清理
----------
用界面上的「结束并清除服务」，或用安装包卸载。手动清理：

   sc stop RemapCopilotKeyAgent
   sc delete RemapCopilotKeyAgent
   schtasks /Delete /TN RemapCopilotKey /F     旧版本遗留的计划任务

本程序不需要安装任何运行时：.NET Framework 4.8 是 Windows 10 1903+ /
Windows 11 系统自带的组件。

文件说明
--------
RemapCopilotKey.exe          配置程序（界面，需要管理员权限）
RemapCopilotKeyService.exe   C++ 服务程序（键盘钩子，后台静默，无窗口无托盘）
RemapCopilotKey.exe.config   配置程序的运行配置，请勿删除
config.json                  首次保存后生成；改完即刻热加载
RemapCopilotKeyService.log   服务程序日志（级别由 config.json 的 logLevel 决定）
RemapCopilotKey.boot.log     启动追踪，不受 logLevel 影响；排查问题先看它

卸载 / 恢复
-----------
在配置程序里点「结束并清除服务」：停止服务并删除开机自启注册，
Copilot 键恢复为原始的 Win+Shift+F23 行为。

项目地址：https://github.com/Anlerway/RemapCopilotKey
"@

# UTF-8 带 BOM：记事本打开才不会乱码
$readmePath = Join-Path $DistDir '使用说明.txt'
[System.IO.File]::WriteAllText($readmePath, $readme, (New-Object System.Text.UTF8Encoding($true)))

$distFiles = Get-ChildItem -LiteralPath $DistDir -File
$distSize  = [math]::Round(($distFiles | Measure-Object Length -Sum).Sum / 1MB, 2)
Write-Ok ("{0} 个文件 / {1} MB" -f $distFiles.Count, $distSize)

# ---------------------------------------------------------------------------
# 3) 清除 Mark-of-the-Web（网络标记）
#
# 从网上下载的 zip 会带"来自 Internet"的标记，解压出来的 exe 也会继承，
# 双击时弹"打开文件 - 安全警告"。这里对本机产物执行 Unblock-File。
#
# 注意：这一步只对**本机产出的文件**有效 —— zip 内部的标记要在解压前解锁 zip
# 才有效，所以对从网上下载的用户不生效。要根治只能做代码签名。
# ---------------------------------------------------------------------------
Write-Step '清除 Mark-of-the-Web'
$unblocked = 0
Get-ChildItem -LiteralPath $ReleaseRoot -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
    try { Unblock-File -LiteralPath $_.FullName -ErrorAction SilentlyContinue; $unblocked++ } catch { }
}
Write-Ok "已处理 $unblocked 个文件"

# ---------------------------------------------------------------------------
# 4) 压缩 + 5) SHA256 校验值
# ---------------------------------------------------------------------------
if (-not $NoZip) {
    Write-Step "压缩 -> $(Split-Path -Leaf $ZipPath)"
    if (Test-Path -LiteralPath $ZipPath) { Remove-Item -LiteralPath $ZipPath -Force }

    Compress-Archive -Path (Join-Path $DistDir '*') -DestinationPath $ZipPath -CompressionLevel Optimal
    # 压缩包自己也要解锁，否则用户解压后里面的文件继承标记
    Unblock-File -LiteralPath $ZipPath -ErrorAction SilentlyContinue

    Write-Ok ("{0}  {1} MB" -f (Split-Path -Leaf $ZipPath), [math]::Round((Get-Item $ZipPath).Length / 1MB, 2))

    Write-Step '生成 SHA256 校验值'
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("# RemapCopilotKey v$Version  SHA256")
    $lines.Add('# 校验：certutil -hashfile <文件名> SHA256')
    $lines.Add('')

    $hashed = Get-ChildItem -LiteralPath $ReleaseRoot -Recurse -File | Sort-Object FullName
    foreach ($file in $hashed) {
        if ($file.FullName -eq $SumsPath) { continue }
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        $rel  = $file.FullName.Substring($ReleaseRoot.Length + 1)
        $lines.Add(("{0}  {1}" -f $hash, $rel))
    }

    [System.IO.File]::WriteAllLines($SumsPath, $lines, (New-Object System.Text.UTF8Encoding($true)))
    Write-Ok "-> $(Split-Path -Leaf $SumsPath)"
}

# ---------------------------------------------------------------------------
# 6) 安装程序（可选）
#
# 没有 Inno Setup 就跳过，**不算失败** —— 绿色包本身已经够用。
# ---------------------------------------------------------------------------
$setupPath = $null
if (-not $NoInstaller) {
    Write-Step '安装程序（可选）'

    if (-not $Iscc) {
        $candidates = @(
            (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
            (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
            'D:\APP\Inno Setup 6\ISCC.exe'
        )
        $Iscc = $candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    }

    $iss = Join-Path $PSScriptRoot 'installer.iss'

    if (-not $Iscc -or -not (Test-Path -LiteralPath $Iscc)) {
        Write-Warn '沒找到 Inno Setup（ISCC.exe），跳过安装程序。绿色包不受影响。'
    }
    elseif (-not (Test-Path -LiteralPath $iss)) {
        Write-Warn "找不到 $iss，跳过安装程序。"
    }
    else {
        New-Item -ItemType Directory -Force -Path $InstallerRoot | Out-Null
        $setupName = "$PackageName-setup"
        Write-Info "ISCC: $Iscc"

        & $Iscc $iss "/DAppSource=$DistDir" "/DAppVersion=$Version" "/DOutputName=$setupName" "/DOutputDir=$InstallerRoot"
        if ($LASTEXITCODE -ne 0) { throw "Inno Setup 打包失败（退出码 $LASTEXITCODE）" }

        $candidate = Join-Path $InstallerRoot "$setupName.exe"
        if (Test-Path -LiteralPath $candidate) {
            Unblock-File -LiteralPath $candidate -ErrorAction SilentlyContinue
            $setupPath = $candidate
            Write-Ok ("{0}  {1} MB" -f (Split-Path -Leaf $candidate), [math]::Round((Get-Item $candidate).Length / 1MB, 2))
        }
        else {
            Write-Warn 'ISCC 报告成功，但没有找到安装程序文件。'
        }
    }
}

# ---------------------------------------------------------------------------
# 7) 汇总
# ---------------------------------------------------------------------------
Write-Step '产物清单'

Write-Host ("  {0,-48} {1,9}" -f 'release\RemapCopilotKey\', "$distSize MB")
Get-ChildItem -LiteralPath $DistDir -File | Sort-Object Name | ForEach-Object {
    Write-Host ("      {0,-44} {1,8:N1} KB" -f $_.Name, ($_.Length / 1KB)) -ForegroundColor DarkGray
}

foreach ($extra in @($ZipPath, $SumsPath, $setupPath)) {
    if ($extra -and (Test-Path -LiteralPath $extra)) {
        $rel = $extra.Substring($ArtifactsRoot.Length + 1)
        Write-Host ("  {0,-48} {1,9:N2} MB" -f $rel, ((Get-Item $extra).Length / 1MB))
    }
}

Write-Host ''
Write-Host '  目标机器要求：Windows 10 1903+ / Windows 11（自带 .NET Framework 4.8），无需安装运行时。' -ForegroundColor DarkGray
Write-Ok '打包完成（未签名）'
