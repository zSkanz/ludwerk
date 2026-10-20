# The online game end to end, run against a package rather than a build tree:
# the same proof as the `mmo_end_to_end` test (`run_mmo_end_to_end.cmake`), its
# manager and its three players' machines the package's `engine-host`.
#
#   powershell -File tests\mmo\run.ps1 [-EngineHost <engine-host.exe>] [-Port 7850]
#
# Without -EngineHost it runs the package `current.txt` names, under the
# brand's build folder. -Port is the front door's; the run takes the sixteen
# after it. Ends with 0 when the proof holds, 1 when it does not; what every
# process said is under `%TEMP%\engine-mmo-end-to-end`.

param(
    [string]$EngineHost,
    [int]$Port = 7850
)

. "$PSScriptRoot/../../scripts/lib/brand.ps1"
$repo = Resolve-Path "$PSScriptRoot/../.."
if (-not $EngineHost) {
    $packages = Join-Path $env:LOCALAPPDATA "$BrandName\build\packages"
    $sha = (Get-Content (Join-Path $packages 'current.txt') -Raw).Trim()
    $folder = Get-ChildItem (Join-Path $packages $sha) -Directory | Select-Object -First 1
    $EngineHost = Join-Path $folder.FullName 'engine-host.exe'
}
if (-not (Test-Path $EngineHost)) {
    Write-Host "[mmo] engine-host not found: $EngineHost" -ForegroundColor Red
    exit 1
}

# CMake runs the proof, as the test does: on PATH, or Visual Studio's own.
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -property installationPath
        $candidate = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $candidate) { $cmake = $candidate }
    }
}
if (-not $cmake) {
    Write-Host '[mmo] no cmake: on PATH, or with Visual Studio' -ForegroundColor Red
    exit 1
}

$work = Join-Path $env:TEMP 'engine-mmo-end-to-end'
Write-Host "[mmo] engine: $EngineHost"
Write-Host "[mmo] ports: $Port to $($Port + 16); what each process said: $work"
$env:SDL_ASSERT = 'abort'
& $cmake "-DHOST=$EngineHost" "-DPROJECT=$repo/tests/mmo" "-DWORKDIR=$work" "-DPORT=$Port" `
    -P "$repo/tests/mmo/run_mmo_end_to_end.cmake"
if ($LASTEXITCODE -ne 0) {
    Write-Host '[mmo] FAIL' -ForegroundColor Red
    exit 1
}
Write-Host '[mmo] OK' -ForegroundColor Green
exit 0
