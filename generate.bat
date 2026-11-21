@echo off
setlocal
pushd "%~dp0"
if not defined SHOW23_PREMAKE set "SHOW23_PREMAKE=premake5"
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\fetch-deps.ps1
if errorlevel 1 goto failed
"%SHOW23_PREMAKE%" generate-buildinfo
if errorlevel 1 goto failed
"%SHOW23_PREMAKE%" %* vs2022
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
