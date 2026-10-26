#Requires -Version 5.1
<#
.SYNOPSIS
    RemapCopilotKey - unified build script.

.DESCRIPTION
    Builds the C++ service, the C# configurator and runs the unit tests.

    Why PowerShell instead of a batch file: cmd.exe parses .cmd using the OEM
    codepage, mishandles LF-only line endings, and chokes on environment
    variables whose names contain parentheses (e.g. %ProgramFiles(x86)% is not
    defined in every host). PowerShell avoids all three problems.

    The MSVC environment is imported from VsDevCmd.bat into this process, so
    cl.exe / link.exe / msbuild.exe can be invoked directly with proper argument
    handling. The configurator is a .NET Framework 4.8 WPF project, so it is
    built by the same MSBuild that ships with Visual Studio - no .NET SDK and
    no NuGet packages are required.

.PARAMETER Task
    all | service | config | test | clean   (default: all)

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 test
#>
[CmdletBinding()]
param(
    [ValidateSet('all', 'service', 'config', 'test', 'clean')]
    [string]$Task = 'all'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
$Repo  = Split-Path -Parent $MyInvocation.MyCommand.Path
$Build = Join-Path $Repo 'build'
$Obj   = Join-Path $Build 'obj'
$Out   = Join-Path $Build 'out'
$SrcService = Join-Path $Repo 'src\Service'
$SrcConfig  = Join-Path $Repo 'src\Configurator'
$SrcTests   = Join-Path $Repo 'src\Configurator.Tests'

function Write-Step  { param([string]$m) Write-Host "`n=== $m ===" -ForegroundColor Cyan }
function Write-Ok    { param([string]$m) Write-Host "[OK] $m" -ForegroundColor Green }
function Write-Fail  { param([string]$m) Write-Host "[x]  $m" -ForegroundColor Red }
function Write-Info  { param([string]$m) Write-Host "[i]  $m" -ForegroundColor DarkGray }

# ---------------------------------------------------------------------------
# Visual Studio discovery
# ---------------------------------------------------------------------------
function Find-VsDevCmd {
    $candidates = @(
        (Join-Path $env:SystemDrive 'Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'),
        (Join-Path $env:SystemDrive 'Program Files\Microsoft Visual Studio\Installer\vswhere.exe')
    )
    $vswhere = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $vswhere) { throw "vswhere.exe not found. Visual Studio does not appear to be installed." }

    # -prerelease 是必须的：只装了 Visual Studio 的预览版（Insiders）时，
    # 不带这个开关 vswhere 什么都不返回，看上去像"没装 VS"。
    $vsRoot = & $vswhere -latest -prerelease -products * -property installationPath
    if (-not $vsRoot) { throw "vswhere found no Visual Studio installation." }
    $vsRoot = ($vsRoot | Select-Object -First 1).Trim()
    $script:VsRoot = $vsRoot

    $devCmd = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $devCmd)) { throw "VsDevCmd.bat not found at: $devCmd" }

    Write-Info "Visual Studio: $vsRoot"
    return $devCmd
}

# Import the MSVC toolchain environment into the current process.
# This also puts msbuild.exe on PATH, which the C# projects need.
function Import-MsvcEnvironment {
    param([string]$VsDevCmd)

    $lines = & cmd.exe /c "call `"$VsDevCmd`" -arch=x64 -host_arch=x64 -no_logo >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) { throw "Failed to run VsDevCmd.bat (exit $LASTEXITCODE)." }

    foreach ($line in $lines) {
        $idx = $line.IndexOf('=')
        if ($idx -le 0) { continue }            # skips driver-letter pseudo vars like "=C:"
        $name  = $line.Substring(0, $idx)
        $value = $line.Substring($idx + 1)
        Set-Item -Path "Env:$name" -Value $value -ErrorAction SilentlyContinue
    }

    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
    if (-not $cl) { throw "cl.exe still not on PATH after importing the MSVC environment." }
    Write-Info "MSVC: $($cl.Source)"

    $msbuild = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if (-not $msbuild) { throw "msbuild.exe still not on PATH after importing the MSVC environment." }
    Write-Info "MSBuild: $($msbuild.Source)"
}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
function New-Directory {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Force -Path $Path | Out-Null
    }
}

function Get-MsBuild {
    $msbuild = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if (-not $msbuild) { throw 'msbuild.exe 不在 PATH 上（VsDevCmd 应该已经加进去了）。' }
    return $msbuild.Source
}

# 32 位 MSBuild，用于编译配置程序（WPF）。
#
# 来历：配置程序以前引用计划任务的类型库（taskschd），处理 COM 引用的
# ResolveComReference 任务是 **x86** 的。用 amd64 的 MSBuild 时，它必须另起一个
# x86 任务宿主进程、再走进程间通信去调用；某些受限环境（例如禁止命名管道的沙箱）里
# 这一步会直接失败：
#
#     error MSB4216: 无法运行"ResolveComReference"任务，因为 MSBuild 无法创建或
#     连接到运行时为"CLR4"、体系结构为"x86"的任务宿主
#
# 那个 COM 引用已经删掉了（开机自启改走 SCM 的 P/Invoke，见 ServiceInstaller.cs），
# 理论上现在用 amd64 也行；但 32 位 MSBuild 仍是最不容易出岔子的选择，所以保留。
# （Visual Studio 自带的 Developer Command Prompt 默认就是 32 位的，行为一致。）
function Get-MsBuildX86 {
    if ($script:VsRoot) {
        $candidate = Join-Path $script:VsRoot 'MSBuild\Current\Bin\MSBuild.exe'
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return (Get-MsBuild)
}

# 扫描 XML 类文件里的非法注释（注释里出现连续两个减号）
#
# 后果非常难查：app.manifest 会因此变成非法 XML，打出来的 exe 双击直接报
# "应用程序的并行配置不正确"，进程在 Main 之前就被终止，没有堆栈也没有日志。
# MSBuild 与 XAML 编译器都不检查 manifest，所以这一步必须自己做。
function Test-XmlComments {
    $check = Join-Path $Repo 'tools\check-xml.ps1'
    if (-not (Test-Path -LiteralPath $check)) {
        Write-Info 'tools\check-xml.ps1 不存在，跳过 XML 注释预检'
        return
    }

    Write-Host "  > $check" -ForegroundColor DarkGray
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $check
    if ($LASTEXITCODE -ne 0) { throw 'XML 注释预检失败（见上面列出的文件与行号）。' }
}

function Invoke-Native {
    param(
        [string]   $Exe,
        [string[]] $Arguments,
        [string]   $What
    )
    Write-Host "  > $Exe $($Arguments -join ' ')" -ForegroundColor DarkGray
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit code $LASTEXITCODE)." }
}

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
function Build-Service {
    Write-Step 'Building C++ service (RemapCopilotKeyService.exe) with MSBuild'

    $proj = Join-Path $SrcService 'RemapCopilotKeyService.vcxproj'
    if (-not (Test-Path -LiteralPath $proj)) { throw "Project not found: $proj" }

    # 用正经的 VS 工程编译，和直接在 Visual Studio 里构建完全一致
    $msbuildArgs = @(
        $proj,
        '/p:Configuration=Release',
        '/p:Platform=x64',
        '/nologo',
        '/v:minimal'
    )
    Invoke-Native -Exe (Get-MsBuild) -Arguments $msbuildArgs -What 'Service build'

    $exe = Join-Path $Out 'Service\RemapCopilotKeyService.exe'
    if (-not (Test-Path -LiteralPath $exe)) {
        throw "MSBuild 报告成功，但没有找到 $exe"
    }
    Write-Ok "Service -> $exe"
}

function Build-Config {
    Write-Step 'Building C# configurator (WPF / .NET Framework 4.8)'

    $proj = Join-Path $SrcConfig 'Configurator.csproj'
    if (-not (Test-Path -LiteralPath $proj)) { throw "Project not found: $proj" }

    $outDir = Join-Path $Out 'Configurator'
    # -restore：net48 工程同样需要 project.assets.json（即使不引用任何 NuGet 包）。
    # 目标框架引用程序集取自本机 VS 自带的 .NET Framework 4.8 Targeting Pack，不联网。
    $msbuildArgs = @(
        $proj,
        '-restore',
        '/p:Configuration=Release',
        "/p:OutDir=$outDir\",
        '/nologo',
        '/v:minimal'
    )
    Invoke-Native -Exe (Get-MsBuildX86) -Arguments $msbuildArgs -What 'Configurator build'

    $exe = Join-Path $outDir 'RemapCopilotKey.exe'
    if (-not (Test-Path -LiteralPath $exe)) { throw "MSBuild 报告成功，但没有找到 $exe" }

    # 检查编译产物里真的嵌进了"要求管理员权限"的清单。
    # 源码里的 app.manifest 正确、但 exe 里嵌的却是别的清单，这种事发生过一次，
    # 表现是"注册开机自启报权限不足"，光看源码完全查不出来。
    $check = Join-Path $Repo 'tools\check-manifest.ps1'
    if (Test-Path -LiteralPath $check) {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $check -ExePath $exe
        if ($LASTEXITCODE -ne 0) { throw '配置程序清单检查失败。' }
    }

    Write-Ok "Configurator -> $outDir\"
}

function Run-Tests {
    Write-Step 'Running unit tests'

    $tObj = Join-Path $Obj 'tests'
    $tOut = Join-Path $Out 'tests'
    New-Directory $tObj
    New-Directory $tOut

    $src = Join-Path $SrcService 'src'
    $tests = Join-Path $SrcService 'tests'

    # -------------------------------------------------------------------
    # 1) C# 侧契约测试。除了自身断言，它还会写出一份样例配置，
    #    供下面的 C++ config 测试读回来，形成跨语言闭环。
    # -------------------------------------------------------------------
    $crossLangConfig = Join-Path $tOut 'csharp_config.json'
    if (Test-Path -LiteralPath $crossLangConfig) {
        Remove-Item -LiteralPath $crossLangConfig -Force
    }

    $csProj = Join-Path $SrcTests 'Configurator.Tests.csproj'
    if (Test-Path -LiteralPath $csProj) {
        $csOut = Join-Path $tOut 'csharp-tests'
        # 这个测试工程直接复用配置程序的源码，同样只需要 VS 自带的 MSBuild
        $msbuildArgs = @(
            $csProj,
            '-restore',
            '/p:Configuration=Release',
            "/p:OutDir=$csOut\",
            '/nologo',
            '/v:minimal'
        )
        Invoke-Native -Exe (Get-MsBuild) -Arguments $msbuildArgs -What 'C# contract test build'

        Write-Host ''
        & (Join-Path $csOut 'ConfigContractTests.exe') $crossLangConfig
        if ($LASTEXITCODE -ne 0) { throw 'C# contract tests FAILED.' }
    }
    else {
        Write-Warning "C# contract test project not found, skipping: $csProj"
    }

    # -------------------------------------------------------------------
    # 2) C++ 测试；config 套件会把 C# 写出的那份配置读回来
    # -------------------------------------------------------------------
    # Json/Keys/Log/Config 是配置层测试要一起编译的公共源文件
    $configDeps = @(
        (Join-Path $src 'Json.cpp'),
        (Join-Path $src 'Keys.cpp'),
        (Join-Path $src 'Log.cpp'),
        (Join-Path $src 'Config.cpp')
    )

    $configExtraArgs = @()
    if (Test-Path -LiteralPath $crossLangConfig) {
        $configExtraArgs = @($crossLangConfig)
    }

    $suites = @(
        [pscustomobject]@{
            Name      = 'json'
            Sources   = @((Join-Path $tests 'json_test.cpp'), (Join-Path $src 'Json.cpp'))
            ExtraArgs = @()
        },
        [pscustomobject]@{
            Name      = 'config'
            Sources   = @((Join-Path $tests 'config_test.cpp')) + $configDeps
            ExtraArgs = $configExtraArgs
        }
    )

    $failed = @()
    foreach ($suite in $suites) {
        $exe = Join-Path $tOut ("{0}_test.exe" -f $suite.Name)
        $clArgs = @(
            '/nologo', '/std:c++20', '/EHsc', '/W4', '/permissive-', '/utf-8',
            '/MT', '/O2', '/DNDEBUG'
        ) + $suite.Sources + @(
            "/Fo$tObj\",
            "/Fd$tObj\$($suite.Name).pdb",
            "/Fe$exe"
        )

        Invoke-Native -Exe 'cl.exe' -Arguments $clArgs -What "$($suite.Name) test build"

        Write-Host ''
        & $exe @($suite.ExtraArgs)
        if ($LASTEXITCODE -ne 0) { $failed += $suite.Name }
    }

    if ($failed.Count -gt 0) {
        throw "Unit tests FAILED: $($failed -join ', ')"
    }
    Write-Ok 'Unit tests passed'
}

function Invoke-Clean {
    Write-Step 'Cleaning'
    if (Test-Path -LiteralPath $Build) {
        Remove-Item -LiteralPath $Build -Recurse -Force
        Write-Info "Removed $Build"
    }
    Write-Ok 'Clean finished'
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
try {
    if ($Task -eq 'clean') { Invoke-Clean; exit 0 }

    Write-Step 'Pre-flight checks'
    Test-XmlComments

    $vsDevCmd = Find-VsDevCmd
    Import-MsvcEnvironment -VsDevCmd $vsDevCmd

    switch ($Task) {
        'service' { Build-Service }
        'config'  { Build-Config }
        'test'    { Run-Tests }
        'all' {
            Build-Service
            Build-Config
            Run-Tests
            Write-Host ''
            Write-Ok 'All targets built.'
        }
    }
    exit 0
}
catch {
    Write-Host ''
    Write-Fail $_.Exception.Message
    exit 1
}
