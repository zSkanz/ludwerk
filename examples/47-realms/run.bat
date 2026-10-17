@echo off
REM Convenience launcher for this example. The gates invoke engine-host directly
REM from CMake, so nothing depends on this file -- it exists so a human does not
REM have to remember where an out-of-tree build put the binary (R14).
REM
REM   run.bat            the game's servers: the manager and every world in
REM                      servers.toml, on this machine, with a console
REM   run.bat play       a player, joining them at 127.0.0.1:7777
REM   run.bat solo ...   the town alone, as a gate boots it
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

REM %~dp0 is this file's own directory and always ends in a backslash, which
REM would escape the closing quote and corrupt the argument -- so it is stripped
REM before the path is passed.
set "ENG_EXAMPLE=%~dp0"
set "ENG_EXAMPLE=%ENG_EXAMPLE:~0,-1%"

if "%~1"=="play" (
    "%ENG_HOST%" "%ENG_EXAMPLE%" --join=127.0.0.1:7777
    exit /b
)
if "%~1"=="solo" (
    "%ENG_HOST%" "%ENG_EXAMPLE%" %2 %3 %4 %5 %6 %7 %8 %9
    exit /b
)
"%ENG_HOST%" --manager="%ENG_EXAMPLE%\servers.toml"
