# Build the optional PC integration archive separately from the engine distribution.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$GdkRoot,
    [string]$Out
)
$ErrorActionPreference = 'Continue'
. "$PSScriptRoot/devshell.ps1"
. "$PSScriptRoot/lib/brand.ps1"
$moduleRepository = Split-Path -Parent $PSScriptRoot
$moduleBuildRoot = if ($env:ENG_BUILD_ROOT) { $env:ENG_BUILD_ROOT } else { Join-Path $env:LOCALAPPDATA "$BrandName/build" }
$moduleBuildRoot = [System.IO.Path]::GetFullPath($moduleBuildRoot)
$moduleSdk = [System.IO.Path]::GetFullPath($GdkRoot)
if (-not (Test-Path -LiteralPath (Join-Path $moduleSdk 'include/XGameRuntimeInit.h'))) { throw 'GdkRoot must name the pinned GDK Windows native directory.' }
$moduleBuild = Join-Path $moduleBuildRoot 'win-msvc-gdk'
$moduleOutput = if ($Out) { [System.IO.Path]::GetFullPath($Out) } else { Join-Path $moduleBuildRoot 'optional-modules' }
$moduleVersion = '2604.5.7903'
$moduleRelative = "modules/microsoft-gdk/$moduleVersion/windows-x64"
$moduleSource = Join-Path $moduleBuildRoot $moduleRelative
$moduleStage = Join-Path $moduleOutput "microsoft-gdk-$moduleVersion-windows-x64"
$moduleDestination = Join-Path $moduleStage $moduleRelative
$moduleVcvars = Get-DeveloperShellEnv
$moduleBatch = Join-Path $env:TEMP "engine-module-$PID.cmd"
@"
@echo off
chcp 65001 >nul
call "$moduleVcvars" >nul || exit /b 1
cmake --preset win-msvc-dev -B "$moduleBuild" -DENG_GDK_ROOT="$moduleSdk" || exit /b 1
cmake --build "$moduleBuild" --target engine_xbox_provider
"@ | Set-Content -LiteralPath $moduleBatch -Encoding ascii
Push-Location $moduleRepository
try {
    & cmd.exe /c $moduleBatch
    if ($LASTEXITCODE -ne 0) { throw 'Optional provider build failed.' }
    New-Item -ItemType Directory -Force -Path $moduleDestination | Out-Null
    $moduleHashes = @{}
    # The engine's own provider and nothing of the vendor's (ADR 0188): the
    # vendor's libraries are found in each developer's own SDK when a game is
    # exported, and never ride in this archive.
    foreach ($moduleFile in @('engine_xbox.dll')) {
        $moduleInput = Join-Path $moduleSource $moduleFile
        Copy-Item -LiteralPath $moduleInput -Destination (Join-Path $moduleDestination $moduleFile) -Force
        $moduleHashes[$moduleFile] = (Get-FileHash -LiteralPath $moduleInput -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $moduleManifest = @{ id = 'microsoft-gdk'; integration = 'xbox'; version = $moduleVersion; abi = 1; files = $moduleHashes } | ConvertTo-Json -Depth 3
    [System.IO.File]::WriteAllText((Join-Path $moduleDestination 'provider.json'), $moduleManifest, [System.Text.UTF8Encoding]::new($false))
    $moduleArchive = Join-Path $moduleOutput "microsoft-gdk-$moduleVersion-windows-x64.zip"
    Compress-Archive -LiteralPath (Join-Path $moduleStage 'modules') -DestinationPath $moduleArchive -Force
    Write-Output $moduleArchive
    Get-FileHash -LiteralPath $moduleArchive -Algorithm SHA256
} finally {
    Pop-Location
    Remove-Item -LiteralPath $moduleBatch -ErrorAction SilentlyContinue
}
