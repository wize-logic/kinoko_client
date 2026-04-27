@echo off
setlocal
cd /d "%~dp0"

set PRESET=%1
if "%PRESET%"=="" set PRESET=debug-win32

echo [compile] configuring (%PRESET%)...
cmake --preset %PRESET% || exit /b 1

echo [compile] building (%PRESET%)...
cmake --build --preset %PRESET% || exit /b 1

echo [compile] done.
