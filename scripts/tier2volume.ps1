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
function Get-Tier2Volume {
    param([string]$Base, [string]$Repo)

    $gitDir = git -C $Repo rev-parse --git-dir
    $common = git -C $Repo rev-parse --git-common-dir
    if ($LASTEXITCODE -ne 0 -or -not $gitDir -or $gitDir -eq $common) {
        return $Base
    }
    $md5 = [System.Security.Cryptography.MD5]::Create()
    try {
        $path = [System.IO.Path]::GetFullPath($Repo).ToLowerInvariant()
        $bytes = $md5.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($path))
    } finally {
        $md5.Dispose()
    }
    $mark = ([System.BitConverter]::ToString($bytes, 0, 4) -replace '-', '').ToLowerInvariant()
    return "$Base-$mark"
}
