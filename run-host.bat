@echo off
:: Run the rPlayHub engine (cdhost.exe) on its own, in this console, with its log on screen.
::
:: Normally you do not need this: the GUI (start-rplay.bat, or the Start-menu entry) starts the
:: engine itself. Use it to watch the engine's output, to run it with flags (--help lists them;
:: arguments are passed through), or to drive it without the GUI (scripts/mirror_check.py).
::
:: Looks for, in order: the staged build (scripts/build-windows.sh), an installed copy, and the
:: bare build output host-c\cdhost.exe -- which needs the MSYS2 UCRT64 DLLs on PATH, e.g.
:: set PATH=C:\msys64\ucrt64\bin;%PATH%
title rPlayHub engine
setlocal
cd /d "%~dp0"

:: The engine uses its userspace TCP/IP stack (lwIP) by default on Windows; setting it is harmless.
if not defined RPLAY_USERSPACE_NET set RPLAY_USERSPACE_NET=1
if not defined RPLAY_KEYFRAME_EVERY_S set RPLAY_KEYFRAME_EVERY_S=0
if not defined RPLAY_FPS set RPLAY_FPS=60
if not defined RPLAY_MAX_BITRATE set RPLAY_MAX_BITRATE=15000000
:: The Developer Disk Image: scripts\fetch-ddi.ps1 puts it in %LOCALAPPDATA%\rplayhub\iOS_DDI,
:: where the engine looks by itself; an older checkout-local copy in deps\iOS_DDI still works.
if not defined RPLAY_DDI if exist "%~dp0deps\iOS_DDI\Restore\BuildManifest.plist" set "RPLAY_DDI=%~dp0deps\iOS_DDI"

set "ENGINE="
if exist "%~dp0build\windows\rPlayHub\cdhost.exe" set "ENGINE=%~dp0build\windows\rPlayHub\cdhost.exe"
if not defined ENGINE if exist "%LOCALAPPDATA%\Programs\rPlayHub\cdhost.exe" set "ENGINE=%LOCALAPPDATA%\Programs\rPlayHub\cdhost.exe"
if not defined ENGINE if exist "%~dp0host-c\cdhost.exe" set "ENGINE=%~dp0host-c\cdhost.exe"

if not defined ENGINE (
    echo cdhost.exe not found. Build it first, in an MSYS2 UCRT64 shell:
    echo     scripts/build-windows.sh
    echo See doc\WINDOWS.md.
    pause
    exit /b 1
)

echo Running %ENGINE% %*
"%ENGINE%" %*
echo.
echo The engine exited with code %ERRORLEVEL%.
pause
