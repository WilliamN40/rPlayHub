@echo off
:: ALTERNATIVE PATH -- the documented Windows build is MSYS2 UCRT64 (scripts/build-windows.sh),
:: which builds the engine and this GUI with one toolchain and stages the DLLs. This script builds
:: only the GUI, with MSVC, and is not exercised by CI; its source list must be kept in step with
:: client-c/Makefile by hand.
::
:: Needs:
::   - MSVC. Run from an "x64 Native Tools Command Prompt for VS", or let this script find
::     Visual Studio with vswhere, or set RPLAYHUB_MSVC_SETUP to a .bat that sets up cl/link
::     (e.g. a portable-msvc install's setup_x64.bat).
::   - deps\imgui, deps\json, deps\sdl2 (VC): scripts\fetch-gui-deps-win.ps1
::   - deps\ffmpeg-dist: the RVRA-patched FFmpeg built with MSVC (scripts/build-ffmpeg-msvc.sh).
::     Refused without its RVRA-PATCHED marker: an unpatched FFmpeg links and runs, and garbles
::     every picture after the first resolution downshift with no warning.
:: Output: client-c\rplay-gui.exe (+ SDL2.dll next to it). It does not build cdhost.exe.
setlocal enabledelayedexpansion
cd /d "%~dp0\.."

echo === Building rPlayHub Windows GUI with MSVC (alternative path) ===

where cl >nul 2>nul
if errorlevel 1 (
    if defined RPLAYHUB_MSVC_SETUP (
        call "%RPLAYHUB_MSVC_SETUP%"
    ) else (
        set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
        if not exist "!VSWHERE!" (
            echo error: cl.exe is not on PATH and vswhere.exe was not found.
            echo Run this from an "x64 Native Tools Command Prompt for VS", or set RPLAYHUB_MSVC_SETUP.
            goto :fail
        )
        set "VSPATH="
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
        if not defined VSPATH (
            echo error: vswhere found no Visual Studio with the C++ x64 tools.
            goto :fail
        )
        call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul
    )
)
where cl >nul 2>nul
if errorlevel 1 (
    echo error: cl.exe still not on PATH after setting up MSVC.
    goto :fail
)

if not exist "deps\ffmpeg-dist\RVRA-PATCHED" (
    echo error: deps\ffmpeg-dist is missing or not RVRA-patched ^(no deps\ffmpeg-dist\RVRA-PATCHED^).
    echo Build it with scripts/build-ffmpeg-msvc.sh. Refusing to link an unpatched decoder.
    goto :fail
)
if not exist "deps\imgui\imgui.cpp" (
    echo error: deps\imgui missing -- run scripts\fetch-gui-deps-win.ps1
    goto :fail
)
if not exist "deps\sdl2\lib\x64\SDL2.lib" (
    echo error: deps\sdl2 ^(VC^) missing -- run scripts\fetch-gui-deps-win.ps1
    goto :fail
)

set CFLAGS=/nologo /c /O2 /Oi /Ot /MD /std:c11 /W3 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /Iclient-c /Ideps\ffmpeg-dist\include /Ideps\sdl2\include
set CXXFLAGS=/nologo /c /O2 /Oi /Ot /MD /std:c++17 /EHsc /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /Iclient-c /Iclient-c\ui /Ideps\ffmpeg-dist\include /Ideps\sdl2\include /Ideps\imgui /Ideps\imgui\backends /Ideps\json

echo Compiling C sources...
cl %CFLAGS% client-c\stream-core.c /Fo:client-c\stream-core.obj
if errorlevel 1 goto :fail
cl %CFLAGS% client-c\audio.c /Fo:client-c\audio.obj
if errorlevel 1 goto :fail

echo Compiling UI sources...
for %%s in (window_effects display_window twin_view fold_view png_decode) do (
    cl %CXXFLAGS% client-c\ui\%%s.cc /Fo:client-c\ui\%%s.obj
    if errorlevel 1 goto :fail
)

echo Compiling ImGui sources...
for %%s in (imgui imgui_draw imgui_tables imgui_widgets) do (
    cl %CXXFLAGS% deps\imgui\%%s.cpp /Fo:client-c\%%s.obj
    if errorlevel 1 goto :fail
)
cl %CXXFLAGS% deps\imgui\backends\imgui_impl_sdl2.cpp /Fo:client-c\imgui_impl_sdl2.obj
if errorlevel 1 goto :fail
cl %CXXFLAGS% deps\imgui\backends\imgui_impl_sdlrenderer2.cpp /Fo:client-c\imgui_impl_sdlrenderer2.obj
if errorlevel 1 goto :fail

echo Compiling rplay-gui.cpp...
cl %CXXFLAGS% client-c\rplay-gui.cpp /Fo:client-c\rplay-gui.obj
if errorlevel 1 goto :fail

echo Compiling resources...
pushd client-c
rc /nologo /fo rplay-gui.res rplay-gui.rc
if errorlevel 1 ( popd & goto :fail )
popd

:: FFmpeg's MSVC static build may name its archives avcodec.lib or libavcodec.a.
set "FFLIB=deps\ffmpeg-dist\lib"
set "FFLIBS="
for %%l in (avcodec swscale swresample avutil) do (
    if exist "%FFLIB%\%%l.lib" (
        set "FFLIBS=!FFLIBS! %FFLIB%\%%l.lib"
    ) else (
        set "FFLIBS=!FFLIBS! %FFLIB%\lib%%l.a"
    )
)

echo Linking rplay-gui.exe...
:: GUI subsystem (SDL2main supplies WinMain), as the MSYS2 build; the manifest comes from
:: rplay-gui.rc, so the linker must not add its own.
link /nologo /SUBSYSTEM:WINDOWS /MANIFEST:NO ^
    client-c\rplay-gui.obj ^
    client-c\stream-core.obj ^
    client-c\audio.obj ^
    client-c\ui\window_effects.obj ^
    client-c\ui\display_window.obj ^
    client-c\ui\twin_view.obj ^
    client-c\ui\fold_view.obj ^
    client-c\ui\png_decode.obj ^
    client-c\imgui.obj ^
    client-c\imgui_draw.obj ^
    client-c\imgui_tables.obj ^
    client-c\imgui_widgets.obj ^
    client-c\imgui_impl_sdl2.obj ^
    client-c\imgui_impl_sdlrenderer2.obj ^
    client-c\rplay-gui.res ^
    deps\sdl2\lib\x64\SDL2.lib ^
    deps\sdl2\lib\x64\SDL2main.lib ^
    %FFLIBS% ^
    ws2_32.lib shell32.lib comdlg32.lib user32.lib gdi32.lib ole32.lib advapi32.lib bcrypt.lib secur32.lib winmm.lib dwmapi.lib ^
    /OUT:client-c\rplay-gui.exe
if errorlevel 1 goto :fail

copy /Y "deps\sdl2\lib\x64\SDL2.dll" "client-c\" >nul

echo Build complete: client-c\rplay-gui.exe (with client-c\SDL2.dll).
echo It needs cdhost.exe next to it or already running; build that with scripts/build-windows.sh.
exit /b 0

:: Top-level exit: "exit /b 1" from inside a ( ) block does not reach the caller of a .bat run
:: without "call" (cmd /c script.bat reports 0), so every failure comes here.
:fail
exit /b 1
