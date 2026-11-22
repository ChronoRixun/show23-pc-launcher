@echo off
setlocal DisableDelayedExpansion
if "%~1"=="" goto usage
if not exist "%~dp0..\show23-pc-launcher.exe" goto missing
"%~dp0..\show23-pc-launcher.exe" --manual-play %*
exit /b %errorlevel%

:usage
echo Usage: play.cmd ^<install^> [--fullscreen] [--no-update-art] [--resolution 720^|1080] [--name ^<player name^>] 1>&2
exit /b 2

:missing
echo Bundled runtime missing. Extract the entire release ZIP. 1>&2
exit /b 1
