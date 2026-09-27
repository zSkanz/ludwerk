# Builds a Ludwerk game into an Android APK and, with -Install, puts it on the
# phone connected over USB and starts it.
#
#     scripts/android-player.ps1 -Project examples/20-platformer -Install
#
# **A developer's wrapper around `ludwerk build --target=android`**, which is the
# exporter (ADR 0104 §5): this script only builds, from this repository, the
# prebuilt player an installation ships -- `libmain.so`, cross-compiled for
# arm64 -- and the host tools the export runs (`assetc`, the SPIR-V shaders),
# then hands over. The APK is `dist/android/<name>-<version>.apk` in the project.
#
# Needs what scripts/install-android.ps1 installs.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Project,
    [switch]$Install
)
. "$PSScriptRoot/lib/brand.ps1"

$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    . (Join-Path $PSScriptRoot 'devshell.ps1')
    $vcvars = Get-DeveloperShellEnv
    $root = if ($env:ENG_BUILD_ROOT) { $env:ENG_BUILD_ROOT } else { Join-Path $env:LOCALAPPDATA "$BrandName\build" }
    $sdk = if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { [Environment]::GetEnvironmentVariable('ANDROID_SDK_ROOT', 'User') }
    if (-not $sdk) { throw 'No Android SDK; run scripts/install-android.ps1' }

    $projectRoot = (Resolve-Path $Project).Path
    $dev = Join-Path $root 'win-msvc-dev'
    $player = Join-Path $root 'android-arm64-player'

    $script = @"
chcp 65001 >nul
call "$vcvars" >nul || exit /b 1
cmake --build "$dev" --target engine_host assetc engine_luauembed || exit /b 1
set ANDROID_SDK_ROOT=$sdk
set ANDROID_HOME=$sdk
cmake -S . -B "$player" -G Ninja -DCMAKE_TOOLCHAIN_FILE="%CD%/cmake/toolchains/android.cmake" -DCMAKE_BUILD_TYPE=Release -DENG_PROFILE=player -DENG_ENABLE_REPLICATION=ON -DENG_BUILD_TESTS=OFF -DENG_HOST_LUAUEMBED="$dev/engine/script/engine_luauembed.exe" >nul || exit /b 1
cmake --build "$player" --target engine_host || exit /b 1
"@
    $temp = Join-Path $env:TEMP "engine-android-player-$PID.cmd"
    Set-Content -Path $temp -Value $script -Encoding ascii
    & cmd.exe /c $temp
    $code = $LASTEXITCODE
    Remove-Item $temp -ErrorAction SilentlyContinue
    if ($code -ne 0) { throw "the Android player failed to build" }

    $env:ANDROID_SDK_ROOT = $sdk
    $env:ENG_BUILD_ROOT = $root
    & (Join-Path $PSScriptRoot 'ludwerk.ps1') build $projectRoot --target=android
    if ($LASTEXITCODE -ne 0) { throw "the Android export of $Project failed" }

    $apk = Get-ChildItem (Join-Path $projectRoot 'dist\android') -Filter '*.apk' |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $apk) { throw "no APK under $projectRoot\dist\android" }
    Write-Host "[android] $($apk.FullName)" -ForegroundColor Green

    if ($Install) {
        $adb = Join-Path $sdk 'platform-tools\adb.exe'
        & $adb install -r $apk.FullName
        if ($LASTEXITCODE -ne 0) { throw 'adb install failed -- is the phone connected, with USB debugging allowed?' }
        # The package the APK says it is, read out of it rather than guessed
        # from project.toml: the export decided it, and this only starts it.
        $aapt = Join-Path $sdk 'build-tools\35.0.0\aapt2.exe'
        $appId = (& $aapt dump packagename $apk.FullName | Select-Object -First 1).Trim()
        if (-not $appId) { throw "could not read the package name out of $($apk.FullName)" }
        & $adb shell am start -n "$appId/engine.player.PlayerActivity" | Out-Null
        Write-Host "[android] started $appId on the phone" -ForegroundColor Green
    }
} finally {
    Pop-Location
}
