# A package a commit, each in a folder of its own, and one pointer to the
# newest.
#
#   <build root>\packages\<sha>\<brand>-<version>-win64\    the package
#   <build root>\packages\current.txt                       the sha to use
#
# **A package folder is written once and never again.** `scripts\package.ps1`
# writes into whatever folder it is given, and for a long time it was given the
# same one every time: a game being captured from that folder, an editor left
# open in it, a tool that ran the engine out of it -- each was running files a
# package step was about to replace, and each time the answer was a rule for
# people to follow (ask before packaging, wait for "whole", copy it first). A
# rule every new session breaks once is not a rule. So the step no longer
# rewrites anything: a host running from an older package goes on running,
# untouched, for as long as it likes.
#
# `current.txt` is replaced in one step after the package has been verified,
# so reading it never finds half a package: what it names is whole.
#
# Old packages are removed here, and only when nothing is running from them,
# and never the newest few.
#
# Usage:
#   scripts\package-step.ps1                 # package HEAD of this checkout
#   scripts\package-step.ps1 -Keep 5         # keep the five newest
#   scripts\package-step.ps1 -PackageArgs '-SkipLinuxPlayer'
#
# To run the newest package from a tool:
#   $root = "$env:LOCALAPPDATA\<brand>\build\packages"
#   $sha  = (Get-Content "$root\current.txt" -Raw).Trim()
#   & (Get-ChildItem "$root\$sha" -Directory | Select-Object -First 1).FullName\engine-host.exe ...

[CmdletBinding()]
param(
    # How many of the newest packages are kept whatever else is true.
    [int]$Keep = 3,
    # Where `packages\` is. Defaults to the brand's build folder -- NOT to
    # $env:ENG_BUILD_ROOT, which a caller may point at a build tree of its own.
    [string]$Root,
    # Passed through to scripts\package.ps1, as one string.
    [string]$PackageArgs = ''
)
. "$PSScriptRoot/lib/brand.ps1"

$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot

if (-not $Root) {
    $Root = Join-Path $env:LOCALAPPDATA "$BrandName\build"
}
$packages = Join-Path $Root 'packages'
New-Item -ItemType Directory -Force -Path $packages | Out-Null

$sha = (& git -C $repo rev-parse --short=8 HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or -not $sha) {
    Write-Host 'package-step: this checkout has no HEAD to name a package by.' -ForegroundColor Red
    exit 1
}
$destination = Join-Path $packages $sha
$marker = Join-Path $destination 'whole.txt'

if (Test-Path -LiteralPath $marker) {
    Write-Host "package-step: $sha is already packaged at $destination; nothing is rewritten." -ForegroundColor DarkGray
}
else {
    # A folder with no marker is a step that did not finish. Nothing can be
    # running a package that was never announced, so it is cleared.
    if (Test-Path -LiteralPath $destination) {
        Remove-Item -LiteralPath $destination -Recurse -Force -Confirm:$false
    }
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'package.ps1'), '-Out', $destination)
    if ($PackageArgs) { $arguments += $PackageArgs.Split(' ', [System.StringSplitOptions]::RemoveEmptyEntries) }
    & powershell @arguments
    if ($LASTEXITCODE -ne 0) {
        Write-Host "package-step: packaging $sha failed; the pointer still names the package before it." -ForegroundColor Red
        exit $LASTEXITCODE
    }
    [System.IO.File]::WriteAllText($marker, "$sha`n", [System.Text.UTF8Encoding]::new($false))
}

# The pointer, in one step: written beside itself and moved over the old one.
$pointer = Join-Path $packages 'current.txt'
$staged = "$pointer.next"
[System.IO.File]::WriteAllText($staged, "$sha`n", [System.Text.UTF8Encoding]::new($false))
Move-Item -LiteralPath $staged -Destination $pointer -Force
Write-Host "package-step: current is $sha" -ForegroundColor Green

# Old ones go, where nobody is in them.
$running = @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath } | ForEach-Object { $_.ExecutablePath })
$folders = @(Get-ChildItem -LiteralPath $packages -Directory | Sort-Object LastWriteTime -Descending)
$kept = 0
foreach ($folder in $folders) {
    if ($folder.Name -eq $sha -or $kept -lt ($Keep - 1)) {
        if ($folder.Name -ne $sha) { $kept += 1 }
        continue
    }
    $prefix = $folder.FullName.TrimEnd('\') + '\'
    $inUse = @($running | Where-Object { $_.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase) }).Count
    if ($inUse -ne 0) {
        Write-Host "package-step: $($folder.Name) is kept -- $inUse process(es) run from it." -ForegroundColor DarkGray
        continue
    }
    Remove-Item -LiteralPath $folder.FullName -Recurse -Force -Confirm:$false
    Write-Host "package-step: removed $($folder.Name)" -ForegroundColor DarkGray
}
exit 0
