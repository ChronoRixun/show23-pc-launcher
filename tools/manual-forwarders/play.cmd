@echo off
setlocal DisableDelayedExpansion
set "version="
for /f "delims=" %%V in ('powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0select-version.ps1"') do set "version=%%V"
if not defined version exit /b 1
"%~dp0..\show23-pc\versions\%version%\manual\%~nx0" %*
