# The editor's distribution archive (ADR 0054).
#
# Build the two profiles a package needs, write the folder, PROVE it works from
# outside this repository, and only then compress it. The order is the point:
# an archive published without the third step is an archive whose first user is
# the person who finds out it cannot find its own engine.
#
# Usage:
#   scripts\package.ps1                 # build, package, verify, archive
#   scripts\package.ps1 -SkipBuild      # the presets are already built
#   scripts\package.ps1 -NoArchive      # leave the folder, skip the zip
#
# Windows, because this is the tier phase one ships. The packager underneath is
# not Windows-specific -- it packages for whatever platform it runs on -- and
# this script is the part that knows about vcvars, `chcp` and Compress-Archive.
#
# **Every target's player goes in** (ADR 0104 §6), so an installation exports
# for Linux and Android with nothing else on the machine: the Linux player is
# built in the Tier-2 container the gate already uses, and copied out of its
# volume; the Android one is cross-built with the NDK, as the gate's `android`
# stage builds it. A machine without Docker or without the NDK packages
# without that player, and says so.

[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$NoArchive,
    # Package without the Linux player (no Docker) or the Android one (no NDK).
    [switch]$SkipLinuxPlayer,
    [switch]$SkipAndroidPlayer,
    # Where the folder and the archive go. Defaults to the packager's own
    # default, which is `$env:ENG_BUILD_ROOT\package`.
    [string]$Out
)
. "$PSScriptRoot/lib/brand.ps1"

# 'Continue' rather than 'Stop', for the reason localgate.ps1 gives at length:
# Windows PowerShell 5.1 turns a native command's stderr into error records, and
# under 'Stop' an ordinary progress line aborts the run. $LASTEXITCODE is the
# only signal from a native tool that means what it says.
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
. "$PSScriptRoot/devshell.ps1"

try {
    if (-not $env:ENG_BUILD_ROOT) {
        $env:ENG_BUILD_ROOT = Join-Path $env:LOCALAPPDATA "$BrandName\build"
    }

    if (-not $SkipBuild) {
        Write-Host "=== build (editor, player) ===" -ForegroundColor Cyan
        $vcvars = Get-DeveloperShellEnv

        # One cmd invocation, because the environment vcvars64.bat establishes
        # does not survive back into PowerShell -- and `chcp 65001` first,
        # because without it ninja records no header dependencies at all and
        # every incremental build reuses stale objects (D040).
        #
        # Named targets rather than the whole tree: what the folder carries is
        # the host, the two tools the CLI shells out to, and the player host.
        # Building the C++ suite on the way would be compiling a hundred
        # translation units this then does not ship, which is the reason the
        # `editor` profile turns them off in the first place.
        $script = @"
chcp 65001 >nul
call "$vcvars" >nul || exit /b 1
cmake --preset win-msvc-editor || exit /b 1
cmake --build --preset win-msvc-editor --target engine_host assetc iconpatch engine_relay || exit /b 1
cmake --preset win-msvc-player || exit /b 1
cmake --build --preset win-msvc-player --target engine_host || exit /b 1
"@
        $temp = Join-Path $env:TEMP "engine-package-$PID.cmd"
        Set-Content -Path $temp -Value $script -Encoding ascii
        try {
            & cmd.exe /c $temp
            if ($LASTEXITCODE -ne 0) { throw "the editor or player build failed" }
        } finally {
            Remove-Item $temp -ErrorAction SilentlyContinue
        }
    }

    $players = Join-Path $env:ENG_BUILD_ROOT 'package-players'
    $linuxPlayer = Join-Path $players 'linux-x64'
    if (-not $SkipLinuxPlayer) {
        Write-Host "=== the Linux player (Tier-2 container) ===" -ForegroundColor Cyan
        docker image inspect engine-tier2:latest *> $null
        if ($LASTEXITCODE -ne 0) {
            throw "no engine-tier2 image; run scripts/localgate.ps1 -Only linux once, or pass -SkipLinuxPlayer"
        }
        docker volume create engine-tier2-build | Out-Null
        if (Test-Path $linuxPlayer) { Remove-Item -Recurse -Force $linuxPlayer }
        New-Item -ItemType Directory -Force $players | Out-Null
        # Built into the gate's own volume, then copied out through a bind
        # mount: the volume is not a directory this machine can read.
        docker run --rm -v "${repo}:/repo" -v "engine-tier2-build:/build" -v "${players}:/out" engine-tier2:latest `
            bash -c "cmake --preset linux-clang-player >/dev/null && cmake --build --preset linux-clang-player --target engine_host engine_relay && mkdir -p /out/linux-x64 && cp /build/linux-clang-player/engine/app/engine-host /build/linux-clang-player/tools/relay/engine-relay /out/linux-x64/ && strip --strip-unneeded /out/linux-x64/engine-host /out/linux-x64/engine-relay && cp -r /build/linux-clang-player/engine/app/content /out/linux-x64/"
        if ($LASTEXITCODE -ne 0) { throw "the Linux player failed to build in the container" }
    }

    $androidPlayer = Join-Path $env:ENG_BUILD_ROOT 'android-arm64-player\engine\app'
    $llvmStrip = $null
    if (-not $SkipAndroidPlayer) {
        Write-Host "=== the Android player (NDK) ===" -ForegroundColor Cyan
        $sdkRoot = if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { [Environment]::GetEnvironmentVariable('ANDROID_SDK_ROOT', 'User') }
        $ndkVersion = (Select-String -Path 'cmake/toolchains/android.cmake' `
                -Pattern '^set\(ENG_ANDROID_NDK_VERSION "([^"]+)"').Matches[0].Groups[1].Value
        if (-not $sdkRoot -or -not (Test-Path (Join-Path $sdkRoot "ndk\$ndkVersion"))) {
            throw "no Android NDK $ndkVersion; run scripts/install-android.ps1, or pass -SkipAndroidPlayer"
        }
        $vcvars = Get-DeveloperShellEnv
        $playerDir = Join-Path $env:ENG_BUILD_ROOT 'android-arm64-player'
        $luauembed = Join-Path $env:ENG_BUILD_ROOT 'win-msvc-dev\engine\script\engine_luauembed.exe'
        if (-not (Test-Path $luauembed)) { throw "the Android player needs win-msvc-dev's engine_luauembed; build win-msvc-dev first" }
        $script = @"
chcp 65001 >nul
call "$vcvars" >nul || exit /b 1
set ANDROID_SDK_ROOT=$sdkRoot
set ANDROID_HOME=$sdkRoot
cmake -S . -B "$playerDir" -G Ninja -DCMAKE_TOOLCHAIN_FILE="%CD%/cmake/toolchains/android.cmake" -DCMAKE_BUILD_TYPE=Release -DENG_PROFILE=player -DENG_ENABLE_REPLICATION=ON -DENG_BUILD_TESTS=OFF -DENG_HOST_LUAUEMBED="$luauembed" >nul || exit /b 1
cmake --build "$playerDir" --target engine_host || exit /b 1
"@
        # **Stripped on the way into the folder.** The Release build keeps its
        # symbols -- 160 MB of them -- and Gradle strips a library only when
        # the NDK is installed, which `ludwerk android install-tools` does not
        # install: an unstripped player would be every exported APK's size.
        $llvmStrip = Join-Path $sdkRoot "ndk\$ndkVersion\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe"
        $temp = Join-Path $env:TEMP "engine-package-android-$PID.cmd"
        Set-Content -Path $temp -Value $script -Encoding ascii
        try {
            & cmd.exe /c $temp
            if ($LASTEXITCODE -ne 0) { throw "the Android player failed to build" }
        } finally {
            Remove-Item $temp -ErrorAction SilentlyContinue
        }
    }

    Write-Host "=== the folder ===" -ForegroundColor Cyan
    $arguments = @('tools/repo/package.luau')
    if ($Out) { $arguments += "--out=$Out" }
    if (-not $SkipLinuxPlayer) { $arguments += "--linux-player=$linuxPlayer" }
    if (-not $SkipAndroidPlayer) { $arguments += "--android-player=$androidPlayer" }
    & lute $arguments
    if ($LASTEXITCODE -ne 0) { throw "the packager reported missing files" }

    $root = if ($Out) { $Out } else { Join-Path $env:ENG_BUILD_ROOT 'package' }
    if ($llvmStrip) {
        $written = Get-ChildItem -Path $root -Directory -Filter "$BrandName-*" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        $library = Join-Path $written.FullName 'player\android-arm64\libmain.so'
        & $llvmStrip --strip-unneeded $library
        if ($LASTEXITCODE -ne 0) { throw "llvm-strip failed on $library" }
    }

    # **Verified before it is compressed.** The suite runs the packaged `ludwerk`
    # from a scratch directory with no ENG_BUILD_ROOT in its environment: if
    # the folder cannot find its own engine, its own template and its own
    # version, there is nowhere else for it to look.
    Write-Host "=== the folder works from outside this repository ===" -ForegroundColor Cyan
    & lute test tests/installed
    if ($LASTEXITCODE -ne 0) { throw "the packaged folder did not pass tests/installed" }

    $folder = Get-ChildItem -Path $root -Directory -Filter "$BrandName-*" |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $folder) { throw "no $BrandName-* folder under $root" }

    if ($NoArchive) {
        Write-Host "package: $($folder.FullName)" -ForegroundColor Green
        return
    }

    Write-Host "=== the archive ===" -ForegroundColor Cyan
    $archive = Join-Path $root "$($folder.Name).zip"
    Remove-Item $archive -ErrorAction SilentlyContinue
    # The suite above has just run the folder's own programs, and a program
    # that ended a moment ago can still be held -- by the process going away,
    # or by a scanner reading a file it has not seen before. The archive then
    # fails on that one file, and it used to say so and return zero with no
    # archive at all. So: a few tries, and a failure that is one.
    $written = $false
    foreach ($attempt in 1..5) {
        Remove-Item $archive -ErrorAction SilentlyContinue
        try {
            Compress-Archive -Path $folder.FullName -DestinationPath $archive -ErrorAction Stop
            $written = $true
            break
        } catch {
            Write-Host "package: the archive did not close (try $attempt): $($_.Exception.Message)" -ForegroundColor Yellow
            Start-Sleep -Seconds 3
        }
    }
    if (-not $written) { throw "the archive could not be written: something holds a file under $($folder.FullName)" }
    $megabytes = [math]::Round((Get-Item $archive).Length / 1MB, 1)
    Write-Host "package: $archive ($megabytes MiB)" -ForegroundColor Green
} finally {
    Pop-Location
}
