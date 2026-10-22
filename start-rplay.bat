@echo off
:: Start the rPlayHub GUI from a source checkout. The GUI starts and stops the engine
:: (cdhost.exe) itself, so there is nothing else to launch -- run-host.bat is only for running
:: the engine on its own in a console.
::
:: Looks for, in order: the staged build (scripts/build-windows.sh), an installed copy, and the
:: bare build output in client-c\.
setlocal
cd /d "%~dp0"

set "GUI="
if exist "%~dp0build\windows\rPlayHub\rplay-gui.exe" set "GUI=%~dp0build\windows\rPlayHub\rplay-gui.exe"
if not defined GUI if exist "%LOCALAPPDATA%\Programs\rPlayHub\rplay-gui.exe" set "GUI=%LOCALAPPDATA%\Programs\rPlayHub\rplay-gui.exe"
if not defined GUI if exist "%~dp0client-c\rplay-gui.exe" set "GUI=%~dp0client-c\rplay-gui.exe"

if not defined GUI (
    echo rplay-gui.exe not found. Build it first, in an MSYS2 UCRT64 shell:
    echo     scripts/build-windows.sh
    echo or install rPlayHub with the installer. See doc\WINDOWS.md.
    pause
    exit /b 1
)

echo Launching %GUI%
start "" "%GUI%" %*
