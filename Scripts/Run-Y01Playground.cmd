@echo off
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Run-Y01Playground.ps1" %*
set "result=%errorlevel%"
if not "%result%"=="0" pause
exit /b %result%
