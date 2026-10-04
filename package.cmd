@echo off
rem ---------------------------------------------------------------------------
rem  RemapCopilotKey - release packaging (portable + framework, zipped).
rem  Real logic lives in tools\package.ps1. ASCII + CRLF only.
rem ---------------------------------------------------------------------------
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\package.ps1" %*
exit /b %errorlevel%
