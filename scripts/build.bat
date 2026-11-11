@echo off
rem Build the reproducer. Run this from a "x64 Native Tools Command Prompt for
rem VS" (or any shell where cl.exe and cmake.exe are on PATH).
rem
rem   scripts\build.bat [Release|Debug] [-DONETBB_GIT_TAG=<sha> ...]
rem
rem The first configure downloads oneTBB at the pinned commit, so it needs
rem network access. Everything is staged in build\dist.
setlocal EnableExtensions

set "ROOT=%~dp0.."
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
shift

set "EXTRA="
:collect
if "%~1"=="" goto :collected
set "EXTRA=%EXTRA% %1"
shift
goto :collect
:collected

cmake -S "%ROOT%" -B "%ROOT%\build" -A x64 %EXTRA%
if errorlevel 1 exit /b 1

cmake --build "%ROOT%\build" --config %CONFIG% --parallel
if errorlevel 1 exit /b 1

echo.
echo Built and staged in %ROOT%\build\dist
echo Run scripts\run.bat to crash it.
exit /b 0
