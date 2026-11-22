@echo off
setlocal DisableDelayedExpansion
if "%~1"=="" goto usage
set "bundle=%~dp0..\show23-pc"
set "version="
if exist "%bundle%\manifests\runtime.txt" set /p "version="<"%bundle%\manifests\runtime.txt"
if not defined version goto missing
set "runtime=%bundle%\runtime\%version%"
if not exist "%runtime%\server\python\python.exe" goto missing
if not exist "%runtime%\emulator\shadPS4.exe" goto missing
"%runtime%\server\python\python.exe" -B "%runtime%\server\olserver.py" play --install %* --emulator "%runtime%\emulator\shadPS4.exe"
exit /b %errorlevel%

:usage
echo Usage: play.cmd ^<install^> [--fullscreen] [--no-update-art] [--resolution 720^|1080] [--name ^<player name^>] 1>&2
exit /b 2

:missing
echo Bundled runtime missing. Extract the entire release ZIP. 1>&2
exit /b 1
