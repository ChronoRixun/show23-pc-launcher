@echo off
setlocal DisableDelayedExpansion
if "%~2"=="" goto usage
if not "%~4"=="" goto usage
set "bundle=%~dp0.."
set "version="
if exist "%bundle%\manifests\runtime.txt" set /p "version="<"%bundle%\manifests\runtime.txt"
if not defined version goto missing
set "server=%bundle%\runtime\%version%\server"
if not exist "%server%\python\python.exe" goto missing
if "%~3"=="" (
    "%server%\python\python.exe" -B "%server%\olserver.py" setup --install "%~1" --base "%~2"
) else (
    "%server%\python\python.exe" -B "%server%\olserver.py" setup --install "%~1" --base "%~2" --update "%~3"
)
exit /b %errorlevel%

:usage
echo Usage: setup.cmd ^<install^> ^<unpacked base^> [unpacked update] 1>&2
exit /b 2

:missing
echo Bundled server missing. Extract the entire release ZIP. 1>&2
exit /b 1
