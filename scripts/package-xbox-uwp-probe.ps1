# Local Xbox Developer Mode feasibility package, deliberately separate from export.
[CmdletBinding()]
param(
    [string]$Out,
    [string]$SdkVersion = '10.0.26100.0',
    [string]$PackageVersion = '0.1.0.4',
    [switch]$NativeRenderer,
    [string]$ShaderContent
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/devshell.ps1"
. "$PSScriptRoot/lib/brand.ps1"
$probeRepository = Split-Path -Parent $PSScriptRoot
$probeExecutable = if ($NativeRenderer) { 'engine-uwp-renderer-check' } else { 'engine-uwp-probe' }
$probeTarget = if ($NativeRenderer) { 'engine_uwp_renderer_check' } else { 'engine_uwp_probe' }
if ($NativeRenderer) {
    if (-not $ShaderContent) { throw 'NativeRenderer requires -ShaderContent from a built host content directory.' }
    foreach ($probeShader in @('debug_line.vertex.dxil', 'debug_line.fragment.dxil')) {
        if (-not (Test-Path -LiteralPath (Join-Path $ShaderContent "shaders/dxil/$probeShader"))) {
            throw "Missing shipping shader: $probeShader"
        }
    }
}
$probeParsedVersion = $null
if (-not [Version]::TryParse($PackageVersion, [ref]$probeParsedVersion) -or
    $probeParsedVersion.Revision -lt 0 -or $probeParsedVersion.Major -gt 65535 -or
    $probeParsedVersion.Minor -gt 65535 -or $probeParsedVersion.Build -gt 65535 -or
    $probeParsedVersion.Revision -gt 65535) { throw 'PackageVersion must have four components in the APPX range (0..65535).' }
$probeBuildRoot = if ($env:ENG_BUILD_ROOT) { $env:ENG_BUILD_ROOT } else { Join-Path $env:LOCALAPPDATA "$BrandName/build" }
$probeBuild = [IO.Path]::GetFullPath((Join-Path $probeBuildRoot 'uwp-x64-probe'))
$probeOutput = if ($Out) { [IO.Path]::GetFullPath($Out) } elseif ($NativeRenderer) {
    Join-Path $probeBuildRoot 'xbox-native-renderer-check'
} else { Join-Path $probeBuildRoot 'xbox-uwp-probe' }
$probeOutput = [IO.Path]::GetFullPath($probeOutput)
$probeSourcePrefix = [IO.Path]::GetFullPath($probeRepository).TrimEnd('\') + '\'
foreach ($probePath in @($probeBuild, $probeOutput)) {
    if ($probePath.StartsWith($probeSourcePrefix, [StringComparison]::OrdinalIgnoreCase) -or
        $probePath -eq $probeSourcePrefix.TrimEnd('\')) { throw 'Probe outputs must be outside the repository.' }
}
$probeSdkBin = Join-Path ${env:ProgramFiles(x86)} "Windows Kits/10/bin/$SdkVersion/x64"
$probeVclibs = Join-Path ${env:ProgramFiles(x86)} 'Microsoft SDKs/Windows Kits/10/ExtensionSDKs/Microsoft.VCLibs/14.0/Appx/Retail/x64/Microsoft.VCLibs.x64.14.00.appx'
foreach ($probeTool in @('makeappx.exe', 'signtool.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $probeSdkBin $probeTool))) { throw "Missing SDK tool: $probeTool" }
}
if (-not (Test-Path -LiteralPath $probeVclibs)) { throw 'The x64 UWP VCLibs redistributable is not installed.' }
$probeVcvars = Join-Path (Split-Path (Get-DeveloperShellEnv)) 'vcvarsall.bat'
$probeBatch = Join-Path $env:TEMP "engine-uwp-probe-$PID.cmd"
@"
@echo off
chcp 65001 >nul
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "$probeVcvars" x64 uwp $SdkVersion >nul || exit /b 1
cmake -S "$probeRepository" -B "$probeBuild" -G Ninja -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0 -DCMAKE_BUILD_TYPE=Release -DENG_BUILD_UWP_PROBE=ON -DENG_COMPILER_CACHE=OFF || exit /b 1
cmake --build "$probeBuild" --target $probeTarget || exit /b 1
"@ | Set-Content -LiteralPath $probeBatch -Encoding ascii
try {
    $ErrorActionPreference = 'Continue'
    & cmd.exe /c $probeBatch
    $probeBuildExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($probeBuildExit -ne 0) { throw 'UWP probe build failed.' }

    # A fresh staging directory prevents old DLLs/assets from entering the package.
    $probeStage = Join-Path $probeBuild ("stage-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path "$probeStage/Assets", "$probeStage/i18n", $probeOutput -Force | Out-Null
    Copy-Item -LiteralPath "$probeBuild/platforms/uwp/$probeExecutable.exe" -Destination $probeStage
    if ($NativeRenderer) {
        New-Item -ItemType Directory -Path "$probeStage/shaders" -Force | Out-Null
        foreach ($probeShader in @('debug_line.vertex.dxil', 'debug_line.fragment.dxil')) {
            Copy-Item -LiteralPath (Join-Path $ShaderContent "shaders/dxil/$probeShader") -Destination "$probeStage/shaders/$probeShader"
        }
    } else {
        Copy-Item -LiteralPath "$probeRepository/platforms/uwp/probe.luau" -Destination $probeStage
    }
    Copy-Item -LiteralPath "$probeRepository/i18n/en.json" -Destination "$probeStage/i18n/en.json"
    Copy-Item -LiteralPath "$probeRepository/LICENSE" -Destination "$probeStage/LICENSE.txt"
    Copy-Item -LiteralPath "$probeRepository/third_party/luau/LICENSE.txt" -Destination "$probeStage/Luau-LICENSE.txt"
    Add-Type -AssemblyName System.Drawing
    $probeIcon = [Drawing.Image]::FromFile("$probeRepository/branding/app-icon-512.png")
    try {
        foreach ($probeLogo in @(@('StoreLogo', 50), @('Square44x44Logo', 44), @('Square150x150Logo', 150))) {
            $probeBitmap = [Drawing.Bitmap]::new([int]$probeLogo[1], [int]$probeLogo[1])
            $probeGraphics = [Drawing.Graphics]::FromImage($probeBitmap)
            try {
                $probeGraphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $probeGraphics.DrawImage($probeIcon, 0, 0, $probeBitmap.Width, $probeBitmap.Height)
                $probeBitmap.Save((Join-Path $probeStage "Assets/$($probeLogo[0]).png"), [Drawing.Imaging.ImageFormat]::Png)
            } finally { $probeGraphics.Dispose(); $probeBitmap.Dispose() }
        }
    } finally { $probeIcon.Dispose() }
    $probeCatalog = Get-Content -LiteralPath "$probeRepository/i18n/en.json" -Raw -Encoding UTF8 | ConvertFrom-Json
    $probeManifest = Get-Content -LiteralPath "$probeRepository/platforms/uwp/AppxManifest.xml.in" -Raw
    $probeManifest = $probeManifest.Replace('@PROBE_VERSION@', $probeParsedVersion.ToString())
    $probeTitle = if ($NativeRenderer) { $probeCatalog.'uwp.renderer_check.title' } else { $probeCatalog.'uwp.probe.title' }
    $probeDescription = if ($NativeRenderer) { $probeCatalog.'uwp.renderer_check.scope' } else { $probeCatalog.'uwp.probe.scope' }
    if ($NativeRenderer) {
        $probeManifest = $probeManifest.Replace('Engine.UwpProbe', 'Engine.UwpRendererCheck')
        # Publisher must continue matching the existing development certificate.
        $probeManifest = $probeManifest.Replace('CN=EngineUwpRendererCheck', 'CN=EngineUwpProbe')
        $probeManifest = $probeManifest.Replace('Id="Probe"', 'Id="RendererCheck"')
        $probeManifest = $probeManifest.Replace('engine-uwp-probe.exe', "$probeExecutable.exe")
    }
    $probeManifest = $probeManifest.Replace('@PROBE_DISPLAY_NAME@', [Security.SecurityElement]::Escape("$BrandName - $probeTitle"))
    $probeManifest = $probeManifest.Replace('@PROBE_PUBLISHER_NAME@', [Security.SecurityElement]::Escape($BrandName))
    $probeManifest = $probeManifest.Replace('@PROBE_DESCRIPTION@', [Security.SecurityElement]::Escape($probeDescription))
    [IO.File]::WriteAllText("$probeStage/AppxManifest.xml", $probeManifest, [Text.UTF8Encoding]::new($false))
    $probePackage = Join-Path $probeOutput "$probeExecutable-x64.appx"
    & "$probeSdkBin/makeappx.exe" pack /d $probeStage /p $probePackage /o
    if ($LASTEXITCODE -ne 0) { throw 'APPX validation/packaging failed.' }

    # The private key remains in this user's certificate store, never in an artifact.
    $probeCert = Get-ChildItem Cert:\CurrentUser\My | Where-Object {
        $_.Subject -eq 'CN=EngineUwpProbe' -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(1) -and
        ($_.EnhancedKeyUsageList.ObjectId -contains '1.3.6.1.5.5.7.3.3')
    } | Sort-Object NotAfter -Descending | Select-Object -First 1
    if (-not $probeCert) {
        $probeCert = New-SelfSignedCertificate -Type Custom -Subject 'CN=EngineUwpProbe' -KeyUsage DigitalSignature `
            -FriendlyName 'Engine UWP probe local development' -CertStoreLocation Cert:\CurrentUser\My `
            -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(1) `
            -KeyExportPolicy NonExportable -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
    }
    & "$probeSdkBin/signtool.exe" sign /fd SHA256 /sha1 $probeCert.Thumbprint /s My $probePackage
    if ($LASTEXITCODE -ne 0) { throw 'Development package signing failed.' }
    Export-Certificate -Cert $probeCert -FilePath "$probeOutput/engine-uwp-probe.cer" -Force | Out-Null
    Copy-Item -LiteralPath $probeVclibs -Destination $probeOutput -Force
    Copy-Item -LiteralPath "$probeRepository/platforms/uwp/README.md" -Destination "$probeOutput/README.md" -Force
    [IO.File]::WriteAllText("$probeBuild/last-stage.txt", $probeStage)
    Get-FileHash -LiteralPath $probePackage -Algorithm SHA256
    Write-Output "Prototype package: $probePackage"
} finally {
    Remove-Item -LiteralPath $probeBatch -ErrorAction SilentlyContinue
}
