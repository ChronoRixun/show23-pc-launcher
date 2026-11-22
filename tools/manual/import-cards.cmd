@echo off
setlocal DisableDelayedExpansion
if "%~1"=="" (
    echo Usage: import-cards.cmd ^<install^> 1>&2
    exit /b 2
)
set "bundle=%~dp0.."
set "version="
if exist "%bundle%\manifests\runtime.txt" set /p "version="<"%bundle%\manifests\runtime.txt"
if not defined version goto missing
set "server=%bundle%\runtime\%version%\server"
if not exist "%server%\python\python.exe" goto missing
"%server%\python\python.exe" -B "%server%\olserver.py" import-cards --install %* --pictures
exit /b %errorlevel%

:missing
echo Bundled server missing. Extract the entire release ZIP. 1>&2
exit /b 1
