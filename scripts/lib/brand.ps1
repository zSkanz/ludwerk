# The product's name, from branding/brand.toml (ADR 0109): the one place it
# lives. Dot-sourced by the scripts that name a folder after the product -- the
# build root under the user's folder, the package folder.
$BrandName = Get-Content (Join-Path $PSScriptRoot '..\..\branding\brand.toml') |
    Where-Object { $_ -match '^name\s*=\s*"([^"]*)"' } |
    ForEach-Object { $Matches[1] } |
    Select-Object -First 1
