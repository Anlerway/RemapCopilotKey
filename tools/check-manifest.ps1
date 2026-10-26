#Requires -Version 5.1
<#
.SYNOPSIS
    构建后检查：配置程序的 exe 里确实嵌入了"要求管理员权限"的清单。

.DESCRIPTION
    为什么需要这个检查：

    配置程序必须与 C++ 服务程序**同级提权**运行，否则调 SCM 注册开机自启服务
    会被系统直接拒绝（错误 5，Access denied）。
    提权要求写在 src\Configurator\app.manifest 里，由编译器用 /win32manifest 嵌进 exe。

    这一段曾经出过一次问题：源码里的 app.manifest 明明是 requireAdministrator，
    但某次构建出来的 exe 里嵌的却是 asInvoker —— 结果是"按钮点了没反应/报权限不足"，
    而看源码完全正常。这类问题光看源码发现不了，**必须检查编译产物**。

    顺带也检查：
      * 清单本身是合法 XML（非法 XML 的清单会让 exe 直接报"并行配置不正确"）
      * 没有意外的 asInvoker 提权级别混进清单里

.PARAMETER ExePath
    要检查的 exe 路径，默认是 build\out\Configurator\RemapCopilotKey.exe。

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\check-manifest.ps1
#>
[CmdletBinding()]
param(
    [string] $ExePath = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $ExePath) {
    $ExePath = Join-Path $repo 'build\out\Configurator\RemapCopilotKey.exe'
}

if (-not (Test-Path -LiteralPath $ExePath)) {
    Write-Host "[x]  找不到 $ExePath，请先构建配置程序。" -ForegroundColor Red
    exit 1
}

# 清单是明文嵌在 PE 资源里的，直接按 UTF-8 从字节流里取出来
$text = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($ExePath))

$start = $text.IndexOf('<?xml')
$end = $text.IndexOf('</assembly>')

if ($start -lt 0 -or $end -le $start) {
    Write-Host '[x]  exe 里找不到嵌入的应用程序清单。' -ForegroundColor Red
    Write-Host '     检查 csproj 里是否还有 <ApplicationManifest>app.manifest</ApplicationManifest>。' -ForegroundColor Yellow
    exit 1
}

$manifest = $text.Substring($start, $end - $start + 12)

# 1) 清单必须是合法 XML
try {
    $doc = New-Object System.Xml.XmlDocument
    $doc.LoadXml($manifest)
} catch {
    Write-Host '[x]  exe 里嵌入的清单不是合法 XML：' -ForegroundColor Red
    Write-Host ('     ' + $_.Exception.Message) -ForegroundColor Red
    Write-Host '     exe 双击会报"应用程序的并行配置不正确"。' -ForegroundColor Yellow
    exit 1
}

# 2) 提权级别必须是 requireAdministrator
$levelNode = $doc.SelectSingleNode('//*[local-name()="requestedExecutionLevel"]')
if ($null -eq $levelNode) {
    Write-Host '[x]  清单里没有 requestedExecutionLevel 节点。' -ForegroundColor Red
    exit 1
}

$level = $levelNode.GetAttribute('level')

if ($level -ne 'requireAdministrator') {
    Write-Host ("[x]  配置程序的提权级别是 '{0}'，期望 'requireAdministrator'。" -f $level) -ForegroundColor Red
    Write-Host '     这会导致 SCM 拒绝注册开机自启服务（错误 5，权限不足）。' -ForegroundColor Yellow
    Write-Host '     请确认 src\Configurator\app.manifest 里是 requireAdministrator，然后重新构建。' -ForegroundColor Yellow
    exit 1
}

# 3) 顺带报告 DPI 设置，确认清单是我们要的那一份
$dpiNode = $doc.SelectSingleNode('//*[local-name()="dpiAwareness"]')
$dpi = if ($null -ne $dpiNode) { $dpiNode.InnerText } else { '(未声明)' }

Write-Host ("[OK] 配置程序清单检查通过：提权={0}，DPI={1}" -f $level, $dpi) -ForegroundColor Green
exit 0
