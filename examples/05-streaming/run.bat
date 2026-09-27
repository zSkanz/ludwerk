@echo off
REM Convenience launcher for this example. The gates invoke engine-host directly
REM from CMake, so nothing depends on this file -- it exists so a human does not
REM have to remember where an out-of-tree build put the binary (R14).
REM
REM Extra arguments pass through, so the headless forms still work:
REM   run.bat --headless --frames=600 --exit --screenshot=out.png
REM
REM See examples/README.md for the convention and how to add one.

setlocal

if not defined ENG_PRESET set "ENG_PRESET=win-msvc-dev"

if not defined ENG_BUILD_ROOT (
    echo [run] ENG_BUILD_ROOT is not set -- run scripts\bootstrap.ps1 once.
    exit /b 1
)

set "ENG_HOST=%ENG_BUILD_ROOT%\%ENG_PRESET%\engine\app\engine-host.exe"

if not exist "%ENG_HOST%" (
    echo [run] engine-host not found at:
    echo        %ENG_HOST%
    echo [run] Build it from a Developer Shell:
    echo        cmake --build --preset %ENG_PRESET%
    exit /b 1
)

set "ENG_EXAMPLE=%~dp0"
set "ENG_EXAMPLE=%ENG_EXAMPLE:~0,-1%"

REM This example's world is GENERATED and not committed (M7 brief, Decision 8):
REM the generator is forty lines and the world it writes is a megabyte and a
REM half. Both steps run here when the output is missing, so a fresh clone works
REM without anybody having to read the README first.
if not exist "%ENG_EXAMPLE%\content\world" (
    echo [run] generating the world...
    lute "%ENG_EXAMPLE%\tools\generate_world.luau" || exit /b 1
)

if not exist "%ENG_EXAMPLE%\.engine\content.chunks.json" (
    echo [run] compiling the world...
    bash "%ENG_EXAMPLE%\..\..\scripts\ludwerk.sh" build-assets "%ENG_EXAMPLE%" || exit /b 1
)

"%ENG_HOST%" "%ENG_EXAMPLE%" %*
