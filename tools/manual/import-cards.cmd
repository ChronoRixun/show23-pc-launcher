@echo off
setlocal DisableDelayedExpansion
if "%~1"=="" (
    echo Usage: import-cards.cmd ^<install^> 1>&2
    exit /b 2
)
if not exist "%~dp0..\show23-pc-launcher.exe" goto missing
"%~dp0..\show23-pc-launcher.exe" --manual-import %*
exit /b %errorlevel%

:missing
echo Bundled runtime missing. Extract the entire release ZIP. 1>&2
exit /b 1
