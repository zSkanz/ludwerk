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

REM **The world is in the scene now** (ADR 0053), so there is nothing to generate
REM or compile before a run. `content\scenes\main.scene.json` holds the island as
REM ordinary instances and the engine partitions it into streamed cells on boot,
REM cached under .engine\partition.
REM
REM This file used to run the generator and assetc when `content\world` was
REM missing. `tools\generate_world.luau` is a SEED rather than a build step: it
REM fills an empty Scenery folder once, and after that the scene is the source
REM and the editor is what edits it.

"%ENG_HOST%" "%ENG_EXAMPLE%" %*
