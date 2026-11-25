@echo off
setlocal DisableDelayedExpansion
if "%~2"=="" goto usage
if not "%~4"=="" goto usage
if not exist "%~dp0..\show23-pc-launcher.exe" goto missing
"%~dp0..\show23-pc-launcher.exe" --manual-setup %*
exit /b %errorlevel%

:usage
echo Usage: setup.cmd ^<install^> ^<unpacked base^> [unpacked update] 1>&2
exit /b 2

:missing
echo Bundled runtime missing. Extract the entire release ZIP. 1>&2
exit /b 1
