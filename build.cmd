@echo off
rem ---------------------------------------------------------------------------
rem  Thin wrapper so the build can be started by double-clicking or from any
rem  shell. The real logic lives in build.ps1 (see that file for the rationale).
rem
rem  ASCII + CRLF only - see .gitattributes.
rem ---------------------------------------------------------------------------
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %errorlevel%
