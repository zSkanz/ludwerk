@echo off
REM Convenience launcher for this example. The gates invoke engine-host directly
REM from CMake, so nothing depends on this file -- it exists so a human does not
REM have to remember where an out-of-tree build put the binary (R14).
REM
REM Extra arguments pass through, so the headless forms still work:
REM   run.bat --headless --frames=120 --exit --screenshot=out.png
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

REM %~dp0 is this file's own directory, so the example runs from any cwd.
REM This example is the single-file project shape: one file mounted as one
REM Script (api-design.md section 4).
"%ENG_HOST%" "%~dp0init.luau" %*
