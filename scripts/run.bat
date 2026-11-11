@echo off
rem Run every scenario and report the exit code of each child process.
rem
rem   scripts\run.bat [dist-directory]
rem
rem The access violation is expected: it is the point of the reproducer. The
rem script always exits 0 so that it does not look like a harness failure.
setlocal EnableExtensions EnableDelayedExpansion

set "DIST=%~1"
if "%DIST%"=="" set "DIST=%~dp0..\build\dist"
if not exist "%DIST%\host.exe" (
    echo error: "%DIST%\host.exe" not found
    echo        build first: scripts\build.bat
    exit /b 1
)

echo Reproducer directory: %DIST%
echo.
call :run close
call :run close --exit
call :run finalize
call :run finalize --exit

echo ------------------------------------------------------------------------
echo Expected against oneTBB f1e7e5c (upstream master before the fix):
echo   close            exit 0
echo   close --exit     exit 0
echo   finalize         access violation 0xC0000005
echo   finalize --exit  exit 0
echo.
echo Against the fixed revision 30337af every scenario exits 0.
echo See README.md for other revisions and for the --control diagnostic.
echo ------------------------------------------------------------------------
exit /b 0

:run
echo ==== host.exe %* ====
"%DIST%\host.exe" %*
set "RC=!ERRORLEVEL!"
if "!RC!"=="-1073741819" (
    echo [result] access violation 0xC0000005
) else if "!RC!"=="0" (
    echo [result] exited normally
) else (
    echo [result] exit code !RC!
)
echo.
exit /b 0
