# The Docker volume a checkout builds its Linux trees in, the containers it
# runs them in and the image it runs. Dot-sourced by
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
# **And a second clone is a second checkout** (2026-10-12). A
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

# **And the containers it runs in** (2026-10-20). The gate's containers were
# named once for the machine, so a second checkout's gate removed the first's
# container while it ran, and two checkouts could not run a Docker stage at
# the same time. A checkout's containers are named as its volumes are.
function Get-Tier2Name {
    param([string]$Base, [string]$Repo, [string]$FirstFile)
    return Get-Tier2Volume -Base $Base -Repo $Repo -FirstFile $FirstFile
}

# **The image a checkout's Dockerfile builds, built only when it changed.**
# Each run used to call `docker build`, and BuildKit asks the registry for the
# base's metadata every time even when every layer is cached: with the
# registry unreachable, an image already on the machine, built from the same
# file, could not be used. The image is tagged with a hash of what it is built
# from -- the Dockerfile and the files it copies -- and a checkout whose hash
# has an image runs that image without asking anyone. Two checkouts with
# different Dockerfiles each have their own image rather than rebuilding one
# tag in turn; `engine-tier2:latest` names the last one built, for a person at
# the command line.
$Tier2ImageSources = @('scripts/docker/tier2.Dockerfile', 'rokit.toml')

function Get-Tier2Image {
    param([string]$Repo)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $all = New-Object System.IO.MemoryStream
        foreach ($source in $Tier2ImageSources) {
            $name = [System.Text.Encoding]::UTF8.GetBytes("$source`n")
            $all.Write($name, 0, $name.Length)
            # Line endings as git stores them: a checkout with CRLF and one
            # with LF build the same image.
            $text = [System.IO.File]::ReadAllText((Join-Path $Repo $source)) -replace "`r`n", "`n"
            $bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
            $all.Write($bytes, 0, $bytes.Length)
        }
        $hash = $sha.ComputeHash($all.ToArray())
    } finally {
        $sha.Dispose()
    }
    $mark = ([System.BitConverter]::ToString($hash, 0, 6) -replace '-', '').ToLowerInvariant()
    return "engine-tier2:$mark"
}

function Initialize-Tier2Image {
    param([string]$Repo)

    $server = docker version --format '{{.Server.Version}}'
    if ($LASTEXITCODE -ne 0 -or -not $server) {
        throw "Docker is not running. Start Docker Desktop, or pass -SkipLinux."
    }
    $image = Get-Tier2Image -Repo $Repo
    # Asked for by id: an image that is not there is an error on stderr, and
    # Windows PowerShell turns stderr into a failure.
    $have = docker images -q $image
    if ($have) {
        Write-Host "[tier2] image ${image}: built from this checkout's Dockerfile already" -ForegroundColor DarkGray
        return $image
    }
    Write-Host "[tier2] image ${image}: building, the Dockerfile or what it copies changed" -ForegroundColor Cyan
    # To the console, not the function's output: the caller takes the image's
    # name from what this returns.
    docker build -f (Join-Path $Repo 'scripts/docker/tier2.Dockerfile') -t $image -t engine-tier2:latest $Repo | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "the Tier-2 image failed to build" }
    return $image
}
