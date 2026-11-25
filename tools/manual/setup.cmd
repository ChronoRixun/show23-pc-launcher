@echo off
setlocal DisableDelayedExpansion
if "%~1"=="" goto usage
if not "%~4"=="" goto usage
if not exist "%~dp0..\show23-pc-launcher.exe" goto missing
if "%~2"=="" goto update
if /i "%~2"=="--rollback" goto update
"%~dp0..\show23-pc-launcher.exe" --manual-setup %*
exit /b %errorlevel%

:update
"%~dp0..\show23-pc-launcher.exe" --manual-update %*
exit /b %errorlevel%

:usage
echo Usage: setup.cmd ^<install^> ^<unpacked base^> [unpacked update] 1>&2
echo        setup.cmd ^<existing install^> [--rollback] 1>&2
exit /b 2

:missing
echo Bundled runtime missing. Extract the entire release ZIP. 1>&2
exit /b 1
