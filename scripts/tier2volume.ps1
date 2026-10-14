# The Docker volume a checkout builds its Linux trees in. Dot-sourced by
# `localgate.ps1` and `package.ps1`.
#
# **One volume a checkout.** The container sees every checkout at the same
# path, /repo, so two checkouts sharing a volume share object files -- and
# ninja keeps an object that is newer than its source, whichever checkout the
# source came from. A package built from a commit in a second worktree, while
# the first was being edited, left objects in the shared player tree that were
# newer than those edits: the next gate did not compile them, and failed at the
# link on a function that was in the source. A change to a header would have
# linked, with two layouts of one struct in the binary.
#
# The first checkout keeps the name the volume always had, so its cache
# stands; a linked worktree gets the name with a mark of its own path.
#
# **And a second clone is a second checkout** (an earlier defect's gate, 2026-10-12). A
# clone has a `.git` of its own, so it does not look like a linked worktree,
# and it was handed the first checkout's volume: a fix gated in a clean clone
# left that volume full of objects built from the clone's sources, and every
# file the first checkout had edited meanwhile had to be touched before its
# next build would compile it. `-FirstFile` names a file on this machine that
# holds the path of the checkout the plain name belongs to -- written by the
# first checkout to ask -- and any other path gets a mark, linked or not.
function Get-Tier2Volume {
    param([string]$Base, [string]$Repo, [string]$FirstFile)

    $path = [System.IO.Path]::GetFullPath($Repo).TrimEnd([char]92, [char]47).ToLowerInvariant()
    $gitDir = git -C $Repo rev-parse --git-dir
    $common = git -C $Repo rev-parse --git-common-dir
    $linked = $LASTEXITCODE -eq 0 -and $gitDir -and $gitDir -ne $common
    if (-not $linked) {
        if (-not $FirstFile) {
            return $Base
        }
        if (-not (Test-Path $FirstFile)) {
            $parent = Split-Path -Parent $FirstFile
            if ($parent -and -not (Test-Path $parent)) {
                New-Item -ItemType Directory -Force $parent | Out-Null
            }
            Set-Content -Path $FirstFile -Value $path -Encoding utf8
        }
        $first = (Get-Content $FirstFile -TotalCount 1).Trim().TrimEnd([char]92, [char]47).ToLowerInvariant()
        if ($first -eq $path) {
            return $Base
        }
    }
    $md5 = [System.Security.Cryptography.MD5]::Create()
    try {
        $bytes = $md5.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($path))
    } finally {
        $md5.Dispose()
    }
    $mark = ([System.BitConverter]::ToString($bytes, 0, 4) -replace '-', '').ToLowerInvariant()
    return "$Base-$mark"
}
