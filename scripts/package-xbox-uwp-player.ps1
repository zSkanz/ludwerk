# Builds the actual engine player with an existing exported game pack.
# Windows SDK tools remain external; no GDK or desktop SDL DLLs are packaged.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$GameExport,
    [Parameter(Mandatory)][string]$HostContent,
    [Parameter(Mandatory)][string]$HostLuauEmbed,
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9][A-Za-z0-9.-]{2,49}$')][string]$Identity,
    [Parameter(Mandatory)][string]$DisplayName,
    [Parameter(Mandatory)][string]$Icon,
    [Parameter(Mandatory)][string]$PackageVersion,
    [Parameter(Mandatory)][string]$Out,
    [string]$PublisherSubject = 'CN=EngineDevelopment',
    [string]$SdkVersion = '10.0.26100.0',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/devshell.ps1"
. "$PSScriptRoot/lib/brand.ps1"
$playerRepository = Split-Path -Parent $PSScriptRoot
$playerVersion = $null
if (-not [Version]::TryParse($PackageVersion, [ref]$playerVersion) -or $playerVersion.Revision -lt 0 -or
    @($playerVersion.Major, $playerVersion.Minor, $playerVersion.Build, $playerVersion.Revision).Where({ $_ -gt 65535 }).Count) {
    throw 'PackageVersion must contain four components in 0..65535.'
}
$playerBuildRoot = if ($env:ENG_BUILD_ROOT) { $env:ENG_BUILD_ROOT } else { Join-Path $env:LOCALAPPDATA "$BrandName/build" }
$playerBuild = [IO.Path]::GetFullPath((Join-Path $playerBuildRoot 'uwp-x64-player'))
$playerOutput = [IO.Path]::GetFullPath($Out)
$playerSourcePrefix = [IO.Path]::GetFullPath($playerRepository).TrimEnd('\') + '\'
foreach ($playerPath in @($playerBuild, $playerOutput)) {
    if ($playerPath.StartsWith($playerSourcePrefix, [StringComparison]::OrdinalIgnoreCase) -or $playerPath -eq $playerSourcePrefix.TrimEnd('\')) {
        throw 'Build and package outputs must be outside the repository.'
    }
}
$playerPack = Join-Path $GameExport 'game/.engine/content.lpack'
$playerSdkBin = Join-Path ${env:ProgramFiles(x86)} "Windows Kits/10/bin/$SdkVersion/x64"
$playerVclibs = Join-Path ${env:ProgramFiles(x86)} 'Microsoft SDKs/Windows Kits/10/ExtensionSDKs/Microsoft.VCLibs/14.0/Appx/Retail/x64/Microsoft.VCLibs.x64.14.00.appx'
foreach ($playerRequired in @($playerPack, $HostLuauEmbed, $Icon, $playerVclibs,
    "$playerSdkBin/makeappx.exe", "$playerSdkBin/signtool.exe",
    "$HostContent/shaders/dxil/rhi_blit.vertex.dxil", "$HostContent/shaders/dxil/rhi_blit.fragment.dxil")) {
    if (-not (Test-Path -LiteralPath $playerRequired)) { throw "Required input missing: $playerRequired" }
}
if (-not $SkipBuild) {
    $playerVcvars = Join-Path (Split-Path (Get-DeveloperShellEnv)) 'vcvarsall.bat'
    $playerBatch = Join-Path $env:TEMP "engine-uwp-player-$PID.cmd"
    @"
@echo off
chcp 65001 >nul
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "$playerVcvars" x64 uwp $SdkVersion >nul || exit /b 1
cmake -S "$playerRepository" -B "$playerBuild" -G Ninja -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0 -DCMAKE_BUILD_TYPE=Release -DENG_BUILD_UWP_PLAYER=ON -DENG_COMPILER_CACHE=OFF -DENG_HOST_LUAUEMBED="$HostLuauEmbed" || exit /b 1
cmake --build "$playerBuild" --target engine_host || exit /b 1
"@ | Set-Content -LiteralPath $playerBatch -Encoding ascii
    try {
        & cmd.exe /c $playerBatch
        if ($LASTEXITCODE -ne 0) { throw 'UWP player compilation failed.' }
    } finally { Remove-Item -LiteralPath $playerBatch -ErrorAction SilentlyContinue }
}
$playerStage = Join-Path $playerBuild ('stage-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path "$playerStage/Assets", "$playerStage/game/.engine", $playerOutput -Force | Out-Null
Copy-Item -LiteralPath "$playerBuild/engine/app/engine-host.exe" -Destination $playerStage
Copy-Item -LiteralPath $playerPack -Destination "$playerStage/game/.engine/content.lpack"
Copy-Item -LiteralPath $HostContent -Destination "$playerStage/content" -Recurse
# The player consumes precompiled DXIL. Cross-compilation tools never belong in APPX.
Copy-Item -LiteralPath "$playerRepository/LICENSE" -Destination "$playerStage/LICENSE.txt"
Add-Type -AssemblyName System.Drawing
$playerImage = [Drawing.Image]::FromFile([IO.Path]::GetFullPath($Icon))
try {
    foreach ($playerLogo in @(@('StoreLogo', 50), @('Square44x44Logo', 44), @('Square150x150Logo', 150))) {
        $playerBitmap = [Drawing.Bitmap]::new([int]$playerLogo[1], [int]$playerLogo[1])
        $playerGraphics = [Drawing.Graphics]::FromImage($playerBitmap)
        try {
            $playerGraphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $playerGraphics.DrawImage($playerImage, 0, 0, $playerBitmap.Width, $playerBitmap.Height)
            $playerBitmap.Save((Join-Path $playerStage "Assets/$($playerLogo[0]).png"), [Drawing.Imaging.ImageFormat]::Png)
        } finally { $playerGraphics.Dispose(); $playerBitmap.Dispose() }
    }
} finally { $playerImage.Dispose() }
$playerManifest = Get-Content -LiteralPath "$playerRepository/platforms/uwp/Player.AppxManifest.xml.in" -Raw
foreach ($playerReplacement in @{ IDENTITY=$Identity; VERSION=$playerVersion.ToString(); TITLE=$DisplayName; PUBLISHER=$BrandName; PUBLISHER_SUBJECT=$PublisherSubject }.GetEnumerator()) {
    $playerManifest = $playerManifest.Replace("@$($playerReplacement.Key)@", [Security.SecurityElement]::Escape($playerReplacement.Value))
}
[IO.File]::WriteAllText("$playerStage/AppxManifest.xml", $playerManifest, [Text.UTF8Encoding]::new($false))
$playerPackage = Join-Path $playerOutput "$Identity-$PackageVersion-x64.appx"
& "$playerSdkBin/makeappx.exe" pack /d $playerStage /p $playerPackage /o
if ($LASTEXITCODE -ne 0) { throw 'APPX validation/packaging failed.' }
$playerCert = Get-ChildItem Cert:\CurrentUser\My | Where-Object {
    $_.Subject -eq $PublisherSubject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(1) -and
    ($_.EnhancedKeyUsageList.ObjectId -contains '1.3.6.1.5.5.7.3.3')
} | Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $playerCert) {
    $playerCert = New-SelfSignedCertificate -Type Custom -Subject $PublisherSubject -KeyUsage DigitalSignature `
        -FriendlyName "$BrandName local development" -CertStoreLocation Cert:\CurrentUser\My `
        -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(1) `
        -KeyExportPolicy NonExportable -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
}
& "$playerSdkBin/signtool.exe" sign /fd SHA256 /sha1 $playerCert.Thumbprint /s My $playerPackage
if ($LASTEXITCODE -ne 0) { throw 'Development signing failed.' }
Export-Certificate -Cert $playerCert -FilePath "$playerOutput/engine-development.cer" -Force | Out-Null
Copy-Item -LiteralPath $playerVclibs -Destination $playerOutput -Force
$playerHash = (Get-FileHash -LiteralPath $playerPackage -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$playerOutput/SHA256.txt", "$playerHash  $([IO.Path]::GetFileName($playerPackage))`n", [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText("$playerBuild/last-stage.txt", $playerStage)
Write-Output "Player package: $playerPackage"
