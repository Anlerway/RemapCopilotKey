#Requires -Version 5.1
<#
.SYNOPSIS
    轻量 XML 预检：找出会让 exe 起不来的非法 XML 注释。

.DESCRIPTION
    为什么需要这个脚本：

    Windows 的**应用程序清单**（app.manifest）也是 XML，而 XML 规范禁止注释里出现
    连续两个减号（"双横线"）。一旦出现，整个清单就是非法 XML，
    系统只报一句

        应用程序的并行配置不正确

    然后进程在 Main 之前就被终止 —— 没有堆栈、没有日志，极难定位。
    本项目就踩过一次：清单注释里提到一个以两个减号开头的命令行参数。

    WPF 的 .xaml 同理（XAML 基于 XML），不过 XAML 编译器会直接报错，比较好发现。

    所以构建前统一扫一遍 XML 类文件里的注释。MSBuild / XAML 编译器**不会**检查
    manifest，而 manifest 恰好是最容易踩雷又最难诊断的那个。

.PARAMETER Root
    要扫描的仓库根目录，默认是脚本上一级目录。

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\check-xml.ps1
#>
[CmdletBinding()]
param(
    [string] $Root = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

# 注意：$PSScriptRoot 在 param 块的默认值里可能还是空的（取决于调用方式），
# 所以放到脚本体里再解析。
if (-not $Root) {
    $Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

# 需要检查注释的文件类型
$include = @('*.manifest', '*.xaml', '*.csproj', '*.vcxproj', '*.config', '*.xml')

# 跳过生成物与构建产物
$exclude = '\\(bin|obj|packages|node_modules|build)\\'

$files = Get-ChildItem -LiteralPath $Root -Recurse -Include $include -File -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch $exclude }

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$problems = @()
$commentPattern = [regex]'(?s)<!--(.*?)-->'
$hyphenPattern  = [regex]'--'

foreach ($file in $files) {
    $text = [System.IO.File]::ReadAllText($file.FullName, $utf8NoBom)

    foreach ($match in $commentPattern.Matches($text)) {
        if (-not $hyphenPattern.IsMatch($match.Groups[1].Value)) { continue }

        # 算出注释所在的起始行号，便于直接跳到出错位置
        $line = ($text.Substring(0, $match.Index) -split "`n").Count
        $problems += [pscustomobject]@{
            File = $file.FullName
            Line = $line
        }
    }
}

if ($problems.Count -eq 0) {
    Write-Host ("[OK] XML 注释检查通过（$($files.Count) 个文件）") -ForegroundColor Green
    exit 0
}

Write-Host "[x]  发现非法的 XML 注释（注释里不能出现连续两个减号）：" -ForegroundColor Red
foreach ($problem in $problems) {
    Write-Host ("     {0}:{1}" -f $problem.File, $problem.Line) -ForegroundColor Red
}
Write-Host ''
Write-Host '带上这句提示：打包出来的 exe 双击会报"应用程序的并行配置不正确"。' -ForegroundColor Yellow
Write-Host '把注释里的连续减号改掉（例如拆成 "- -" 或改写措辞）即可。' -ForegroundColor Yellow
exit 1
