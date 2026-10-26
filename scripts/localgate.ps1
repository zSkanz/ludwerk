# The local gate: everything CI checks that can be checked on this machine.
#
# The gates, run here rather than on a hosted runner, for one reason:
#
#   Speed. A missing package in a container is a three-minute discovery here and
#   a ten-minute one there -- and the second kind gets batched, which is how a
#   break survives to the next commit.
#
# What it CANNOT do is macOS. Nothing local can, so Tier-3 stays on CI at the
# milestone gate.
#
# Usage:
#   scripts/localgate.ps1              # everything -- what you run before a push
#   scripts/localgate.ps1 -Only docs   # one stage: docs | luau | format | windows | linux | shipping
#   scripts/localgate.ps1 -Only asan   # address and UB sanitizers, opt-in (see below)
#   scripts/localgate.ps1 -Only winprofiles   # the three profiles a Windows release
#                                             # ships, on MSVC, opt-in (see below)
#   scripts/localgate.ps1 -Only lavapipe      # the real-image goldens, on Mesa's
#                                             # software rasterizer, opt-in
#   scripts/localgate.ps1 -Only lavapipe -Record   # ...rewrite those goldens
#   scripts/localgate.ps1 -Only nakama        # the game's Nakama in containers, pinned,
#                                             # and the Windows build against it; opt-in
#   scripts/localgate.ps1 -Only android       # the engine cross-built for Android
#                                             # arm64 with the pinned NDK; part of
#                                             # every run once scripts/install-android.ps1
#                                             # has installed it
#   scripts/localgate.ps1 -SkipLinux   # ONLY when Docker is genuinely unavailable
#   scripts/localgate.ps1 -Only format -Fix   # rewrite the C++ tree instead of checking it
#   scripts/localgate.ps1 -AllowSkips         # ONLY on a machine with no GPU
#   scripts/localgate.ps1 -Only windows -Tests 'render|terrain_far'
#                                             # THE INNER LOOP: build, then only the
#                                             # tests matching the regex (ctest -R);
#                                             # the full run once per push
#   scripts/localgate.ps1 -Serial             # the stages one after another, as
#                                             # before ADR 0148, to read one log
#   scripts/localgate.ps1 -Only perf          # the tests that measure time, alone,
#                                             # against the trees already built --
#                                             # what the full run does last
#   scripts/localgate.ps1 -Only linux -Perf   # -Only windows and -Only linux leave
#                                             # those tests out; -Perf runs them in
#                                             # the stage, on a machine known quiet
#   scripts/localgate.ps1 -Affected           # THE INNER LOOP AND EACH PUSH OF A
#                                             # BATCH (ADR 0249): only the stages and
#                                             # tests this change can affect, since
#                                             # the merge base with origin/dev
#                                             # (-Base <rev> for another), each with
#                                             # why; the full run once per batch
#
# **The full run is three lanes at once** (ADR 0148): the docs lint alone; this
# machine's own build -- luau, then windows, then android, which needs the
# windows stage's tools; and the container's -- format, then linux, then
# shipping, inside Docker's half of the CPUs. Each lane is this script run
# again with `-Stages`, its log kept and printed whole when it ends, and the
# run is as long as its longest lane.
#
# **A skipped test is a failure here.** Six gates in this repository answer
# `ENG_TEST_SKIP` when there is no graphics device, and ctest counts a skip as
# a pass -- so a GPU that stopped coming up would turn all six green with no
# alarm, and the pixel gates, the two-worlds proof and the settings differential
# would all quietly stop meaning anything. This machine has a device; if a test
# skips on it, something is wrong with the machine or with the test. -AllowSkips
# is for a machine that genuinely has no GPU, and typing it is the point:
# accepting the loss should be a decision, not a default.
#
# The Linux stage is about twelve seconds warm and is not redundant with the
# Windows one: Clang diagnoses things MSVC does not, warnings are errors, and it
# has already caught a defect that would otherwise have reached CI. Skipping it
# to go faster is a false economy -- use -Only for that.

[CmdletBinding()]
param(
    [switch]$SkipLinux,
    [ValidateSet('docs', 'luau', 'format', 'windows', 'linux', 'shipping', 'asan', 'tsan', 'winprofiles', 'lavapipe', 'android', 'nakama', 'perf')]
    [string]$Only,
    # Only meaningful with -Only format: reformat in place rather than report.
    # Off by default, because a gate that edits your tree without being asked is
    # not a gate.
    [switch]$Fix,
    # Accept skipped tests instead of failing on them. See the note above.
    [switch]$AllowSkips,
    # Only meaningful with -Only lavapipe: rewrite the goldens rather than
    # compare against them. A flag, and never something a comparison run can do
    # on its own -- a gate that rewrites its own expectation is not a gate.
    [switch]$Record,
    # The stages one after another in this process, as the gate ran before
    # ADR 0148.
    [switch]$Serial,
    # With -Only windows: build, and run only the ctest entries matching this
    # regex -- the inner loop of ADR 0148. The full run is the outer one.
    [string]$Tests,
    # A lane: these stages, in this order, in this process. What the full run
    # starts three of; not usually typed.
    [string[]]$Stages,
    # The tests that measure time (`LABELS perf`) are left out of the windows
    # and linux stages, for the `perf` stage to run by themselves. What the
    # full run passes its lanes; not usually typed.
    [switch]$PerfApart,
    # With -Only windows or -Only linux: run the tests that measure time in
    # the stage too. Left out by default -- a stage run by itself shares the
    # machine with whatever else is building, and a time measured then is
    # the other work's.
    [switch]$Perf,
    # **Only what this change can affect** (ADR 0249): the stages and the
    # tests `tools/repo/affected.luau` says the files changed since the
    # merge base with -Base reach, each with why. The full run is once per
    # batch, and on CI at every push.
    [switch]$Affected,
    [string]$Base = 'origin/dev',
    # The plan an -Affected run wrote, handed to its lanes; not usually typed.
    [string]$Plan
)
. "$PSScriptRoot/lib/brand.ps1"
# `-File` hands a lane its stages as one string; a lane is a list.
if ($Stages) { $Stages = @($Stages | ForEach-Object { $_ -split ',' } | Where-Object { $_ }) }

# **The stages that run in the Tier-2 container, as a list -- and a run says
# which of them it will use, and when it has stopped.** "Does anything
# still need Docker" was a thing remembered, and was remembered wrongly on the
# day it mattered: Docker's disk was to be moved between two runs, and a step
# that builds in the container was started after Docker had been said to be
# free. A list is read; a memory is trusted. `perf` is here for
# its Linux half. `scripts/package.ps1` says the same of its Linux player.
$ContainerStages = @('format', 'linux', 'shipping', 'perf', 'asan', 'tsan', 'lavapipe')
function Get-ContainerUse {
    # What this invocation runs: one stage, or the full run's eight.
    $will = if ($Only) { @($Only) } else { @('docs', 'luau', 'windows', 'android', 'format', 'linux', 'shipping', 'perf') }
    if ($SkipLinux) { return @() }
    return @($will | Where-Object { $ContainerStages -contains $_ })
}
# Said by the run a person started, not by each lane it starts.
if (-not $Stages) {
    $inContainer = Get-ContainerUse
    if ($inContainer.Count -gt 0) {
        Write-Host "[gate] uses the container (Docker must stay up until this run says it is done with it): $($inContainer -join ', ')" -ForegroundColor Cyan
    } else {
        Write-Host "[gate] nothing of this run uses the container" -ForegroundColor Cyan
    }
}
function Write-ContainerDone {
    if ((Get-ContainerUse).Count -gt 0) {
        Write-Host "[gate] done with the container: nothing of this run uses Docker from here" -ForegroundColor Cyan
    }
}

# 'Continue', not 'Stop', and this is not laziness. Windows PowerShell 5.1 turns
# a native command's stderr into error records; under 'Stop' the first line of
# Docker's buildkit progress -- which it writes to stderr on a perfectly healthy
# build -- aborted the stage before anything compiled, and reported the progress
# line itself as the error.
#
# Every stage below therefore checks $LASTEXITCODE and raises its own exception.
# The exit code is the only signal from a native tool that means what it says.
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
. "$PSScriptRoot/tier2volume.ps1"
# One build volume a checkout: see `tier2volume.ps1` for what sharing one cost.
$firstCheckout = (Join-Path $env:LOCALAPPDATA "$BrandName\tier2-first-checkout.txt")
$buildVolume = Get-Tier2Volume -Base 'engine-tier2-build' -Repo $repo -FirstFile $firstCheckout
$asanVolume = Get-Tier2Volume -Base 'engine-tier2-asan' -Repo $repo -FirstFile $firstCheckout
$tsanVolume = Get-Tier2Volume -Base 'engine-tier2-tsan' -Repo $repo -FirstFile $firstCheckout
# And one name a container a checkout, so two checkouts run their Docker
# stages at the same time.
$gateContainer = Get-Tier2Name -Base 'engine-tier2-gate' -Repo $repo -FirstFile $firstCheckout
$perfContainer = Get-Tier2Name -Base 'engine-tier2-perf' -Repo $repo -FirstFile $firstCheckout
$asanContainer = Get-Tier2Name -Base 'engine-tier2-asan' -Repo $repo -FirstFile $firstCheckout
$tsanContainer = Get-Tier2Name -Base 'engine-tier2-tsan' -Repo $repo -FirstFile $firstCheckout
$shippingContainer = Get-Tier2Name -Base 'engine-shipping-gate' -Repo $repo -FirstFile $firstCheckout
$lavapipeContainer = Get-Tier2Name -Base 'engine-lavapipe-gate' -Repo $repo -FirstFile $firstCheckout
$script:tier2Image = $null

$script:failures = @()
$script:results = @()

# **A test that skips is a test that did not run, and ctest calls that a pass.**
#
# Six gates here answer `ENG_TEST_SKIP` when no graphics device comes up --
# the screenshot golden, the settings differential, the two-worlds editor-seam
# proof and three more. On a machine with a GPU every one of them is supposed to
# execute, so a skip means the device stopped being found and six gates went
# green while measuring nothing. That failure is silent by construction, which is
# exactly the kind this repository writes checks for.
#
# The list of what skipped is printed rather than only counted, because "one
# test skipped" sends somebody looking and "screenshot_gate skipped" tells them
# what they lost.
function Assert-NoSkips {
    param([string]$Log)

    if (-not (Test-Path $Log)) { return }
    $skipped = Select-String -Path $Log -Pattern '^\s*\d+/\d+\s+Test\s+#\d+:\s+(\S+)\s+\.+\*+Skipped' -AllMatches |
        ForEach-Object { $_.Matches[0].Groups[1].Value }

    if (-not $skipped -or $skipped.Count -eq 0) { return }

    $names = ($skipped | Sort-Object -Unique) -join ', '
    if ($AllowSkips) {
        Write-Host "[gate] $($skipped.Count) test(s) skipped, accepted by -AllowSkips: $names" -ForegroundColor Yellow
        return
    }

    throw ("$($skipped.Count) test(s) SKIPPED, and ctest counts a skip as a pass: $names`n" +
        "        On a machine with a graphics device these are supposed to run. A skip here means " +
        "the device stopped being found and those gates went green while measuring nothing.`n" +
        "        If this machine genuinely has no GPU, re-run with -AllowSkips and accept that.")
}

function Invoke-Stage {
    param([string]$Name, [scriptblock]$Body)

    if ($Only -and $Only -ne $Name) { return }
    if ($Stages -and -not ($Stages -contains $Name)) { return }
    # Both container stages answer to the same switch: -SkipLinux means "Docker
    # is not available here", and the formatting gate runs in that same image
    # because that is where the pinned clang-format lives.
    if (($Name -eq 'linux' -or $Name -eq 'format' -or $Name -eq 'shipping' -or $Name -eq 'lavapipe') -and $SkipLinux) { return }
    # A stage this change cannot affect (ADR 0249), said with why.
    $decided = if ($script:affectedPlan) { $script:affectedPlan.stages.$Name } else { $null }
    if ($decided -and -not $decided.run) {
        Write-Host ""
        Write-Host "=== $Name === not run: $($decided.why)" -ForegroundColor DarkGray
        $script:results += "  skip  $Name ($($decided.why))"
        return
    }

    Write-Host ""
    Write-Host "=== $Name ===" -ForegroundColor Cyan
    $watch = [Diagnostics.Stopwatch]::StartNew()

    # Each stage reports its own failure rather than aborting the run: a gate
    # that stops at the first problem hides the other three, and the point of
    # running locally is to learn everything in one pass.
    #
    # Success is "did not throw", never `$?`. Windows PowerShell 5.1 sets `$?`
    # from whether a native command wrote to stderr, not from its exit code --
    # and Docker's buildkit writes its progress there, so `$?` reported every
    # successful image build as a failure. Each stage below raises explicitly on
    # a non-zero exit code, which is the only signal that means anything here.
    # A stage that finds nothing to run says so, and is not counted as a pass:
    # "ok android" on a machine with no NDK claimed a build that
    # never happened.
    $script:stageSkipped = $false
    try {
        & $Body
        $ok = $true
    } catch {
        Write-Host $_.Exception.Message -ForegroundColor Red
        $ok = $false
    }
    $watch.Stop()

    $seconds = [math]::Round($watch.Elapsed.TotalSeconds, 1)
    if ($ok -and $script:stageSkipped) {
        $script:results += "  skip  $Name"
    } elseif ($ok) {
        $script:results += "  ok    $Name ($seconds s)"
    } else {
        $script:results += "  FAIL  $Name ($seconds s)"
        $script:failures += $Name
    }
}

# Git Bash, which ships with Git for Windows, runs the same shell scripts CI
# runs. Using the very same files -- not a Windows transcription of them -- is
# what keeps "it passes locally" meaningful.
function Get-BashPath {
    $candidates = @(
        "$env:ProgramFiles\Git\bin\bash.exe",
        "${env:ProgramFiles(x86)}\Git\bin\bash.exe"
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    $found = Get-Command bash.exe -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }
    throw "bash not found. Install Git for Windows, or run with -Only to skip the shell gates."
}

# `Get-DeveloperShellEnv`, shared with `package.ps1` rather than written twice.
. "$PSScriptRoot/devshell.ps1"

# Two stages want the image now -- the formatting gate and the Tier-2 build --
# so it is built once and reused. Docker's layer cache makes the second call
# free; what this avoids is a second copy of the error handling, which is the
# part that was subtle (see the stderr note above).
# Built once a Dockerfile, and only then (`tier2volume.ps1`).
function Use-Tier2Image {
    $script:tier2Image = Initialize-Tier2Image -Repo $repo
}

# --- What this change can affect (ADR 0249) -----------------------------------
#
# Asked once, by the run a person started, of CTest's listing of this
# machine's tree and of git; its lanes read the plan it wrote. A plan that
# cannot be made is the full suite, said so.
$script:affectedPlan = $null
if ($Affected -and -not $Plan) {
    $Plan = Join-Path $env:TEMP "engine-affected-$PID.json"
    $listing = Join-Path $env:TEMP "engine-affected-tests-$PID.json"
    $vcvarsPlan = Get-DeveloperShellEnv
    if (-not $env:ENG_BUILD_ROOT) {
        $env:ENG_BUILD_ROOT = Join-Path $env:LOCALAPPDATA "$BrandName\build"
    }
    $planScript = @"
chcp 65001 >nul
call "$vcvarsPlan" >nul || exit /b 1
cmake --preset win-msvc-dev >nul || exit /b 1
ctest --preset win-msvc-dev --show-only=json-v1 > "$listing" 2>nul
"@
    $planTemp = Join-Path $env:TEMP "engine-affected-$PID.cmd"
    Set-Content -Path $planTemp -Value $planScript -Encoding ascii
    & cmd.exe /c $planTemp
    $listed = $LASTEXITCODE
    Remove-Item $planTemp -ErrorAction SilentlyContinue
    if ($listed -eq 0) {
        & lute tools/repo/affected.luau $listing --base $Base --out $Plan
        $listed = $LASTEXITCODE
    }
    Remove-Item $listing -ErrorAction SilentlyContinue
    if ($listed -ne 0) {
        Write-Host "[gate] affected: no plan could be made; the full suite" -ForegroundColor Yellow
        $Plan = ''
    }
}
if ($Plan) {
    $script:affectedPlan = Get-Content $Plan -Raw | ConvertFrom-Json
    # The tests that measure time are their own stage's, which the plan asks
    # for or not.
    $PerfApart = $true
}

# **A stage run by itself leaves the tests that measure time out** (ADR
# 0148, amended 2026-10-19), unless -Perf asks for them: two lanes of work on
# one machine meant a single stage was almost never run on a quiet one.
if ($Only -in @('windows', 'linux') -and -not $Perf) {
    $PerfApart = $true
}

# --- The full run: three lanes at once (ADR 0148) -----------------------------
#
# A full run is not run here: it starts three copies of this script, each with
# the stages of one lane, and waits for them. Every lane's log is written to a
# file and printed whole, in a fixed order, when all three are done -- so the
# output reads as one gate rather than three interleaved ones. A lane's stages
# report as they always did; the parent reads their `ok` and `FAIL` lines.
if (-not $Only -and -not $Stages -and -not $Serial) {
    $lanes = [ordered]@{
        'docs'      = @('docs')
        'host'      = @('luau', 'windows', 'android')
        'container' = @('format', 'linux', 'shipping')
    }
    # The real-image goldens, which the full run leaves to be asked for,
    # where a change to the renderer asks (ADR 0249).
    if ($script:affectedPlan -and $script:affectedPlan.stages.lavapipe.run) { $lanes['container'] += 'lavapipe' }
    if ($script:affectedPlan) { Write-Host "[gate] affected: $($script:affectedPlan.summary)" -ForegroundColor Cyan }
    $self = Join-Path $PSScriptRoot 'localgate.ps1'
    $started = [Diagnostics.Stopwatch]::StartNew()
    $running = @()
    foreach ($lane in $lanes.Keys) {
        $log = Join-Path $env:TEMP "engine-localgate-lane-$lane-$PID.txt"
        $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$self`"", '-Stages', ($lanes[$lane] -join ','))
        if ($SkipLinux) { $arguments += '-SkipLinux' }
        if ($AllowSkips) { $arguments += '-AllowSkips' }
        if ($Plan) { $arguments += @('-Plan', "`"$Plan`"") }
        # The tests that measure time wait for the lanes to be over (below).
        $arguments += '-PerfApart'
        Write-Host "[gate] lane $lane`: $($lanes[$lane] -join ' -> ')" -ForegroundColor Cyan
        $process = Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -NoNewWindow -PassThru `
            -RedirectStandardOutput $log -RedirectStandardError "$log.err"
        # Read once now: Windows PowerShell reports a started process's exit code
        # only if its handle was taken while it ran, and as nothing otherwise.
        $null = $process.Handle
        $running += [pscustomobject]@{ Lane = $lane; Process = $process; Log = $log }
    }
    # **The tests that measure time, when the lanes are over, and alone.**
    # Three lanes are six stages on one machine at once: a wall-clock mean
    # taken then is a measure of the other five -- `perf_budget` said a
    # terrain bench took 17.4 ms against a ceiling of 16 in a run where every
    # bench was two to four times its usual, and 9.5 by itself. So the lanes
    # leave them out, and they are run here, with nothing else of the gate
    # running; the stage says so in its own output, so a number is never read
    # without knowing what ran beside it.
    foreach ($entry in $running) { $entry.Process.WaitForExit() }
    $lanes['perf'] = @('perf')
    $perfLog = Join-Path $env:TEMP "engine-localgate-lane-perf-$PID.txt"
    $perfArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$self`"", '-Stages', 'perf')
    if ($SkipLinux) { $perfArguments += '-SkipLinux' }
    if ($Plan) { $perfArguments += @('-Plan', "`"$Plan`"") }
    Write-Host "[gate] lane perf: the tests that measure time, alone, now the other lanes are over" -ForegroundColor Cyan
    $perfProcess = Start-Process -FilePath 'powershell.exe' -ArgumentList $perfArguments -NoNewWindow -PassThru `
        -RedirectStandardOutput $perfLog -RedirectStandardError "$perfLog.err"
    $null = $perfProcess.Handle
    $running += [pscustomobject]@{ Lane = 'perf'; Process = $perfProcess; Log = $perfLog }

    $results = @()
    $failed = @()
    foreach ($entry in $running) {
        $entry.Process.WaitForExit()
        Write-Host ""
        Write-Host "##### lane $($entry.Lane) #####" -ForegroundColor Cyan
        if (Test-Path $entry.Log) { Get-Content $entry.Log }
        if (Test-Path "$($entry.Log).err") { Get-Content "$($entry.Log).err" | Where-Object { $_ -notmatch '^\s*$' } }
        $lines = if (Test-Path $entry.Log) { Get-Content $entry.Log } else { @() }
        $tail = $false
        foreach ($line in $lines) {
            if ($line -match '^=== local gate ===') { $tail = $true; continue }
            if ($tail -and $line -match '^\s+(ok|FAIL|skip)\s+(\S+)') {
                $results += $line
                if ($Matches[1] -eq 'FAIL') { $failed += $Matches[2] }
            }
        }
        if ($entry.Process.ExitCode -ne 0 -and -not ($failed | Where-Object { $lanes[$entry.Lane] -contains $_ })) {
            $failed += "lane $($entry.Lane)"
        }
        Remove-Item $entry.Log, "$($entry.Log).err" -ErrorAction SilentlyContinue
    }
    $started.Stop()
    Pop-Location
    Write-Host ""
    Write-ContainerDone
    Write-Host "=== local gate ===" -ForegroundColor Cyan
    $results | ForEach-Object { Write-Host $_ }
    Write-Host "  wall  $([math]::Round($started.Elapsed.TotalSeconds, 1)) s, three lanes at once and then the tests that measure time alone"
    if ($failed.Count -gt 0) {
        Write-Host ""
        Write-Host "FAILED: $($failed -join ', ')" -ForegroundColor Red
        exit 1
    }
    Write-Host ""
    if ($SkipLinux) {
        Write-Host "green, but the Linux tier did not run -- Clang has not seen this change" -ForegroundColor Yellow
    } else {
        Write-Host "green (macOS is Tier-3 and only CI can build it)" -ForegroundColor Green
    }
    exit 0
}

Invoke-Stage 'docs' {
    & (Get-BashPath) 'scripts/gates/docs-lint.sh'
    if ($LASTEXITCODE -ne 0) { throw "docs-lint failed" }
    # **And once more as a clone runs it.** `docs-lint.sh` runs the checks a
    # checkout adds of its own when `ENG_EXTRA_CHECKS` names them, and a clone
    # and the hosted runner have none: the gate is run again without them, so
    # that what only a clone meets is met here first.
    # Not in a run of what a change affects: the batch's full run does it.
    if ($env:ENG_EXTRA_CHECKS -and -not $script:affectedPlan) {
        $extra = $env:ENG_EXTRA_CHECKS
        Remove-Item Env:ENG_EXTRA_CHECKS
        try {
            & (Get-BashPath) 'scripts/gates/docs-lint.sh'
            if ($LASTEXITCODE -ne 0) { throw "docs-lint failed as a clone runs it (no extra checks)" }
        } finally {
            $env:ENG_EXTRA_CHECKS = $extra
        }
    }
}

Invoke-Stage 'luau' {
    & (Get-BashPath) 'scripts/gates/luau-check.sh'
    if ($LASTEXITCODE -ne 0) { throw "luau-check failed" }
}

Invoke-Stage 'format' {
    # In the container rather than on this machine, because clang-format's
    # output changes between major versions: Visual Studio ships 20 here, Ubuntu
    # 24.04 -- and therefore both the Tier-2 image and `ubuntu-latest` -- ships
    # 18, and a tree formatted by one is unformatted to the other. The gate
    # script refuses any major but its pin for that reason, so running it on the
    # host would fail against a correctly formatted tree.
    Use-Tier2Image

    $arguments = @('scripts/gates/clang-format.sh')
    if ($Fix) { $arguments += '--fix' }

    docker run --rm -v "${repo}:/repo" $script:tier2Image bash @arguments
    if ($LASTEXITCODE -ne 0) { throw "the C++ formatting gate failed" }
}

Invoke-Stage 'windows' {
    # A running `engine-host` holds its own .exe open, so the link fails with
    # LNK1168 -- and the file's timestamp has already moved by then, so the NEXT
    # build considers it current and does not retry. Ninja then reports success
    # and CTest runs a binary from before the change.
    #
    # That cost four wasted cycles and one wrong measurement in the first renderer before it was
    # understood, which is why this is a guard rather than a note: the orphans
    # come from a run whose output was piped into something that exited early
    # (`tail` and `head` are warned against for a different symptom of the same
    # thing), and a gate that silently measures yesterday's binary is worse than
    # one that refuses to start.
    #
    # **Only the hosts that run from this build's own folder**: they are
    # the only ones that hold the executable the link writes. Stopped by name,
    # every engine-host on the machine went with them -- a packaged game being
    # tested, a server somebody else started -- ended from outside with exit -1, no
    # line in their log and no dump, whenever a gate started.
    $vcvars = Get-DeveloperShellEnv
    if (-not $env:ENG_BUILD_ROOT) {
        $env:ENG_BUILD_ROOT = Join-Path $env:LOCALAPPDATA "$BrandName\build"
    }
    $ownBinaries = [System.IO.Path]::GetFullPath((Join-Path $env:ENG_BUILD_ROOT 'win-msvc-dev')) + [System.IO.Path]::DirectorySeparatorChar
    $stale = Get-Process -Name 'engine-host' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($ownBinaries, [System.StringComparison]::OrdinalIgnoreCase) }
    if ($stale) {
        Write-Host "[gate] $(@($stale).Count) engine-host process(es) of this build still running; they would hold the executable open." -ForegroundColor Yellow
        $stale | Stop-Process -Force
        Start-Sleep -Milliseconds 200
    }

    # One cmd invocation, because the environment vcvars64.bat establishes does
    # not survive back into PowerShell.
    #
    # **`chcp 65001` is load-bearing and not cosmetic**. CMake writes
    # ninja`s `msvc_deps_prefix` -- the string ninja looks for in `/showIncludes`
    # output to learn which headers an object depends on -- as UTF-8. A LOCALISED
    # MSVC emits that string with non-ASCII characters in the console codepage,
    # so under any other codepage the two never match, ninja records NO header
    # dependencies at all, and every incremental build silently reuses objects
    # compiled against an older header. It cost this project four debugging
    # sessions before anybody looked at why.
    # `--no-tests=error` because an empty suite is not a passing suite: a preset
    # that stopped registering tests would otherwise report success having run
    # nothing, which is the same shape of lie the skip check below exists for.
    $ctestLog = Join-Path $env:TEMP "engine-localgate-ctest-$PID.txt"
    # **The build knows which headers its objects read**: asked of
    # one object after every build. A build that recorded none -- a codepage
    # that does not match the compiler's note, a launcher that rewrites it --
    # goes on reusing objects compiled against old headers, and says nothing.
    $buildDir = Join-Path $env:ENG_BUILD_ROOT 'win-msvc-dev'
    $depsLog = Join-Path $env:TEMP "engine-localgate-deps-$PID.txt"
    # **Every core** (ADR 0148): the tests that measure time run alone and the
    # ones that draw take three GPU slots (`engine/app/CMakeLists.txt`), and
    # the rest share the machine.
    $ctestJobs = [Environment]::ProcessorCount
    # CTest's own listing of this tree, for `tools/repo/skiplists.luau` below.
    $skipListing = Join-Path $env:TEMP "engine-localgate-tests-$PID.json"
    $ctestFilter = if ($Tests) {
        "-R `"$Tests`""
    } elseif ($script:affectedPlan -and $script:affectedPlan.regex) {
        "-R `"$($script:affectedPlan.regex)`""
    } else { '' }
    # The tests that measure time: left to the `perf` stage where the full run
    # asked for that, and said to have run here where it did not. Those that
    # are `quiet` run in that stage alone, whatever is asked (ADR 0148).
    $ctestFilter = if ($PerfApart) { "$ctestFilter -LE perf" } else { "$ctestFilter -LE quiet" }
    $script = @"
chcp 65001 >nul
call "$vcvars" >nul || exit /b 1
cmake --preset win-msvc-dev || exit /b 1
cmake --build --preset win-msvc-dev || exit /b 1
ninja -C "$buildDir" -t deps engine/core/CMakeFiles/engine_core.dir/src/log.cpp.obj > "$depsLog" 2>&1
ctest --preset win-msvc-dev --show-only=json-v1 > "$skipListing" 2>nul
ctest --preset win-msvc-dev --output-on-failure --no-tests=error -j $ctestJobs $ctestFilter > "$ctestLog" 2>&1 || (type "$ctestLog" & exit /b 1)
type "$ctestLog"
"@
    $temp = Join-Path $env:TEMP "engine-localgate-$PID.cmd"
    Set-Content -Path $temp -Value $script -Encoding ascii
    try {
        & cmd.exe /c $temp
        if ($LASTEXITCODE -ne 0) { throw "the Windows build or tests failed" }
        $deps = if (Test-Path $depsLog) { Get-Content $depsLog -Raw } else { '' }
        if ($deps -notmatch '#deps [1-9]') {
            throw ("the build recorded no header dependencies for log.cpp.obj, so a header edit would not rebuild what includes it:`n" + $deps)
        }
        Assert-NoSkips -Log $ctestLog
        if ($PerfApart) {
            Write-Host "[gate] windows: the tests that measure time were left out; the perf stage runs them alone." -ForegroundColor DarkGray
        } elseif (-not $Tests) {
            Write-Host "[gate] windows: the tests that measure time ran INSIDE this stage -- one at a time among its tests, beside whatever else this machine was doing. Their numbers are this stage's, not a quiet machine's." -ForegroundColor Yellow
        }
        # **And every test that may skip on a machine with no graphics card is
        # on the workflow's lists of tests allowed to.** This machine has one,
        # so a new picture gate missing from those lists is green here and red
        # on a hosted runner after the push.
        if (Test-Path $skipListing) {
            & (Get-BashPath) -c "lute tools/repo/skiplists.luau '$($skipListing -replace '\\', '/')'"
            if ($LASTEXITCODE -ne 0) { throw "a test that may skip is missing from the workflow's lists (see above)" }
        }
    } finally {
        Remove-Item $skipListing -ErrorAction SilentlyContinue
        Remove-Item $temp -ErrorAction SilentlyContinue
        Remove-Item $ctestLog -ErrorAction SilentlyContinue
        Remove-Item $depsLog -ErrorAction SilentlyContinue
    }
    # The inner loop stops at the tests it was asked for.
    if ($Tests) { return }
    # A step this change cannot affect (ADR 0249), said with why.
    $stepRuns = {
        param([string]$Step)
        $decided = if ($script:affectedPlan) { $script:affectedPlan.steps.$Step } else { $null }
        if ($decided -and -not $decided.run) {
            Write-Host "[gate] windows: $Step not run: $($decided.why)" -ForegroundColor DarkGray
            return $false
        }
        return $true
    }

    # The CLI's own path to the same suite. The tooling loop's gate wants `ludwerk test` green
    # on both tiers, and it is a different path from ctest's: it launches the
    # engine, reads the per-case report and emits TAP. ctest proves the engine;
    # this proves the tool a developer types.
    if (& $stepRuns 'conformance') {
        $tap = Join-Path $env:TEMP "engine-test-$PID.tap"
        & (Get-BashPath) 'scripts/ludwerk.sh' test tests/conformance | Set-Content -Path $tap -Encoding utf8
        if ($LASTEXITCODE -ne 0) { throw "ludwerk test failed" }
        Get-Content $tap -Tail 1
        Remove-Item $tap -ErrorAction SilentlyContinue
    }

    # The tooling loop's gate's first item: a dev server, this build headless against it, a
    # file mutated by the test, and the reload confirmed over the WebSocket.
    if (& $stepRuns 'hotreload') {
        & lute test tests/hotreload
        if ($LASTEXITCODE -ne 0) { throw "the hot-reload gate failed" }
    }
    if (-not (& $stepRuns 'packaging')) { return }

    # The flagship work's packaging chain: `ludwerk new`, `ludwerk build`, the built folder RUN with
    # no arguments, and the icon read back out of the artifact. Here rather than
    # in ctest because it drives the CLI, which is a Lute application -- and
    # because `--target win64` is a Windows artifact, which is this stage.
    #
    # **A player tree that is on this machine is brought up to date first.**
    # `ludwerk build` packages the player it finds, with the asset compiler
    # from the same tree -- and a `win-msvc-player` tree is built by the
    # profiles stage, not by this one, so it sat days behind: the suite then
    # packaged a player that could not read what the build had just learned
    # to write, with an `assetc` that did not know the command it was given.
    # A machine with no such tree packages the dev host, as CI does.
    $playerTree = Join-Path $env:ENG_BUILD_ROOT 'win-msvc-player'
    if (Test-Path (Join-Path $playerTree 'build.ninja')) {
        $playerScript = @"
chcp 65001 >nul
call "$vcvars" >nul || exit /b 1
cmake --build --preset win-msvc-player --target engine_host assetc || exit /b 1
"@
        $playerTemp = Join-Path $env:TEMP "engine-player-$PID.cmd"
        Set-Content -Path $playerTemp -Value $playerScript -Encoding ascii
        try {
            & cmd.exe /c $playerTemp
            if ($LASTEXITCODE -ne 0) { throw "the player tree on this machine did not build" }
        } finally {
            Remove-Item $playerTemp -ErrorAction SilentlyContinue
        }
    }
    & lute test tests/packaging
    if ($LASTEXITCODE -ne 0) { throw "the packaging gate failed" }
}

Invoke-Stage 'linux' {
    # No stderr redirection on any of these. Windows PowerShell 5.1 wraps a
    # native command's stderr in an ErrorRecord, which under
    # $ErrorActionPreference='Stop' throws even when the exit code is zero --
    # so `docker rm -f <nonexistent>` used to kill this stage in four seconds,
    # before a single object was compiled.
    Use-Tier2Image

    # A named volume, so the second run is incremental. This is the local
    # equivalent of CI's build cache, and it costs nothing.
    docker volume create $buildVolume | Out-Null
    # The tests inside take block 0 of the test ports: a container has a
    # network of its own, so two never meet there (`cmake/engine_test_ports.cmake`).
    Write-Host "[gate] linux: container $gateContainer, volume $buildVolume, image $script:tier2Image" -ForegroundColor DarkGray

    # Asked for by id first: removing a container that is not there is an error
    # on stderr, and see above for why that matters here.
    $existing = docker ps -aq --filter "name=^${gateContainer}$"
    if ($existing) { docker rm -f $gateContainer | Out-Null }

    # Named and not --rm, so the container and its full log stay visible in
    # Docker Desktop after it exits -- a disposable container takes its own
    # evidence with it.
    $perfMode = if ($PerfApart) { 'without' } else { 'with' }
    # The tests this change can affect (ADR 0249), or every one.
    $gateTests = if ($script:affectedPlan -and $script:affectedPlan.regex) { $script:affectedPlan.regex } else { '' }
    docker run --name $gateContainer `
        -e "ENG_GATE_PERF=$perfMode" `
        -e "ENG_GATE_TESTS=$gateTests" `
        -v "${repo}:/repo" `
        -v "${buildVolume}:/build" `
        $script:tier2Image bash scripts/gates/linux-build.sh
    if ($LASTEXITCODE -ne 0) { throw "the Tier-2 build or tests failed" }
    if ($PerfApart) {
        Write-Host "[gate] linux: the tests that measure time were left out; the perf stage runs them alone." -ForegroundColor DarkGray
    } else {
        Write-Host "[gate] linux: the tests that measure time ran INSIDE this stage -- one at a time among its tests, beside whatever else this machine was doing. Their numbers are this stage's, not a quiet machine's." -ForegroundColor Yellow
    }
}

# **The tests that measure time, by themselves** (`LABELS perf`). What the full
# run starts when its three lanes are over: on Windows, then in the Tier-2
# container, each against the tree its stage built -- nothing is built here.
# Asked for by name (`-Only perf`) it does the same, and what it says of the
# machine is then the caller's to make true. In a `-Serial` run the windows
# and linux stages have run them already, one stage at a time.
Invoke-Stage 'perf' {
    if (-not $Only -and -not $Stages) {
        Write-Host '[gate] perf: nothing to do -- in a serial run the windows and linux stages ran the tests that measure time themselves.' -ForegroundColor DarkGray
        $script:stageSkipped = $true
        return
    }
    $vcvars = Get-DeveloperShellEnv
    if (-not $env:ENG_BUILD_ROOT) {
        $env:ENG_BUILD_ROOT = Join-Path $env:LOCALAPPDATA "$BrandName\build"
    }
    Write-Host '[gate] perf (windows): ALONE -- no other stage of the gate is running.' -ForegroundColor Cyan
    $perfScript = @"
chcp 65001 >nul
call "$vcvars" >nul || exit /b 1
ctest --preset win-msvc-dev --output-on-failure --no-tests=error -L perf || exit /b 1
"@
    $perfTemp = Join-Path $env:TEMP "engine-localgate-perf-$PID.cmd"
    Set-Content -Path $perfTemp -Value $perfScript -Encoding ascii
    try {
        & cmd.exe /c $perfTemp
        if ($LASTEXITCODE -ne 0) { throw "a test that measures time failed on Windows, run alone" }
    } finally {
        Remove-Item $perfTemp -ErrorAction SilentlyContinue
    }
    if ($SkipLinux) { return }
    Write-Host '[gate] perf (linux): ALONE -- no other stage of the gate is running.' -ForegroundColor Cyan
    # A run of this stage alone, or a lane that built no image, has none yet:
    # `docker run` was handed nothing where the image goes, took `bash` for the
    # image's name, and ran the public `bash` image, which has no repository
    # in it ("/scripts/gates/linux-build.sh: No such file or directory").
    if (-not $script:tier2Image) { Use-Tier2Image }
    $existing = docker ps -aq --filter "name=^${perfContainer}$"
    if ($existing) { docker rm -f $perfContainer | Out-Null }
    docker run --name $perfContainer `
        -e ENG_GATE_PERF=only `
        -v "${repo}:/repo" `
        -v "${buildVolume}:/build" `
        $script:tier2Image bash scripts/gates/linux-build.sh
    if ($LASTEXITCODE -ne 0) { throw "a test that measures time failed in the Tier-2 container, run alone" }
}

# **The sanitizers, and they are opt-in for one reason only: they are slow.**
#
# `linux-clang-asan` has been fully wired -- configure, build and test presets,
# with `ENG_SANITIZE=address,undefined` -- and run by nothing at all. A preset
# nobody executes is the same shape of hole as a gate that passes by skipping:
# it looks like coverage in the file and is none on the machine.
#
# It is not in the default run because an instrumented build is two to three
# times slower to compile and to execute, and the standing gate is something a
# person runs before every push. It belongs where slow gates belong, which is
# the nightly job -- `.github/workflows/nightly.yml` runs it there -- and here
# behind `-Only asan` for the afternoon somebody is chasing a lifetime bug.
#
# What it is expected to find, from this repository's own history: one defect was a
# leaked IO slot, and the three-stage decode pipelines hand jobs pointers into
# buffers whose lifetime is the whole argument for how they are written. Those
# are exactly the defects ASan reports and no other gate here can.
#
# Its own build volume, so an instrumented tree never shares object files with
# the ordinary Tier-2 one.
Invoke-Stage 'asan' {
    # **Opt-in, and this line is what makes it so.** `Invoke-Stage` runs every
    # stage when no `-Only` is given, which would have put a two-to-three-times
    # slower instrumented build into the gate a person runs before every push.
    if (-not $Only) {
        Write-Host '[gate] asan: skipped (opt-in; run scripts/localgate.ps1 -Only asan)' -ForegroundColor DarkGray
        $script:stageSkipped = $true
        return
    }
    Use-Tier2Image
    docker volume create $asanVolume | Out-Null

    $existing = docker ps -aq --filter "name=^${asanContainer}$"
    if ($existing) { docker rm -f $asanContainer | Out-Null }

    # `detect_leaks=0`: this is a leak-check-free run on purpose. The engine
    # holds process-lifetime singletons -- the message catalog, the job pool,
    # SDL's own state -- and reporting those as leaks on every run would bury
    # the reports that matter. What is wanted here is the memory-error half:
    # use-after-free, buffer overflow, and the undefined-behaviour checks.
    #
    # `halt_on_error=1` so the first report is the exit code rather than a line
    # in a log nobody reads.
    docker run --name $asanContainer `
        -e ENG_LINUX_PRESET=linux-clang-asan `
        -e ENG_GATE_PERF=without `
        -e ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1 `
        -e ASAN_SYMBOLIZER_PATH=/usr/lib/llvm-18/bin/llvm-symbolizer `
        -e UBSAN_SYMBOLIZER_PATH=/usr/lib/llvm-18/bin/llvm-symbolizer `
        -e UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 `
        -v "${repo}:/repo" `
        -v "${asanVolume}:/build" `
        $script:tier2Image bash scripts/gates/linux-build.sh
    if ($LASTEXITCODE -ne 0) { throw "the sanitizer build or tests failed" }
}

# **ThreadSanitizer, over the tests of what runs on threads**: the job pool,
# the core's queues, the scene's parallel steps, the network, replication,
# the audio pump, the renderer's pipeline workers without a GPU, and the app's
# data workers and sessions. A wait told on a stack that had already returned
# hung macOS for weeks before a sample found it; TSan reports that kind of
# race the first time it happens, where a hang needs it to happen badly.
#
# Opt-in like `asan`, for the same reason, and nightly in
# `.github/workflows/nightly.yml`. Its own volume: a TSan tree shares nothing
# with an ASan one or the ordinary one. What it may not report is in
# `cmake/tsan-suppressions.txt` -- libraries outside the build, and nothing of
# ours.
Invoke-Stage 'tsan' {
    if (-not $Only) {
        Write-Host '[gate] tsan: skipped (opt-in; run scripts/localgate.ps1 -Only tsan)' -ForegroundColor DarkGray
        $script:stageSkipped = $true
        return
    }
    Use-Tier2Image
    docker volume create $tsanVolume | Out-Null

    $existing = docker ps -aq --filter "name=^${tsanContainer}$"
    if ($existing) { docker rm -f $tsanContainer | Out-Null }

    # `halt_on_error=1` so the first race is the exit code; the second stack of
    # a lock-order report is what tells which two locks.
    #
    # **`seccomp=unconfined`, for this container alone.** TSan's runtime turns
    # address randomisation off for the process it runs in, through
    # `personality(ADDR_NO_RANDOMIZE)`, when the kernel randomises more bits of
    # an address than its shadow memory allows -- and Docker's default seccomp
    # profile refuses that call, so the first instrumented program the build
    # runs (the code generator) aborted in the runtime's own check.
    docker run --name $tsanContainer `
        --security-opt seccomp=unconfined `
        -e ENG_LINUX_PRESET=linux-clang-tsan `
        -e ENG_GATE_PERF=without `
        -e "ENG_GATE_TESTS=^(core|jobs|scene|net|replication|audio|render|app)$" `
        -e "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:suppressions=/repo/cmake/tsan-suppressions.txt" `
        -e TSAN_SYMBOLIZER_PATH=/usr/lib/llvm-18/bin/llvm-symbolizer `
        -v "${repo}:/repo" `
        -v "${tsanVolume}:/build" `
        $script:tier2Image bash scripts/gates/linux-build.sh
    if ($LASTEXITCODE -ne 0) { throw "the ThreadSanitizer build or tests failed" }
}

# The one profile no other stage builds, and therefore the one that spent an
# unknown number of commits not compiling at all. It is here rather than
# folded into the Windows stage for two reasons: the check is a shell script, so
# both callers can run the same file, and Clang with
# warnings-as-errors is the stricter reader of the `#if`s that only this profile
# takes.
#
# **It builds three profiles, not one** (and `editor` since): `shipping`,
# `player` and `editor` -- the three nothing else here compiles. `player` is
# what `ludwerk build` packages, the Luau compiler ON and the debug overlay OFF,
# a combination no other profile has; `editor` is the reverse pairing and is
# what a released editor archive is built from. Without this each would be a
# shipped artifact no gate ever built, which is the objection that ruled out
# shipping the `shipping` profile in the first place.
#
# This comment said TWO until 2026-08-26, and so did the error message below --
# a stage that names fewer profiles than it builds is one whose failure sends
# somebody looking in two places when the fault is in a third.
#
# Cost: `engine_host` alone from each, compiled and linked, and not the tree
# around them. The reasoning is in the script, and the short version is that
# only what ENG_LUAU_COMPILER and ENG_DEBUG_UI gate can rot differently
# here, and all of it is reachable from those executables. A few seconds warm,
# one full Release build of the vendored tree cold.
#
# Last, because it is the stage most likely to be cold, and a run that fails
# should say so before spending that.
Invoke-Stage 'shipping' {
    Use-Tier2Image
    docker volume create $buildVolume | Out-Null

    $existing = docker ps -aq --filter "name=^${shippingContainer}$"
    if ($existing) { docker rm -f $shippingContainer | Out-Null }
    Write-Host "[gate] shipping: container $shippingContainer, volume $buildVolume, image $script:tier2Image" -ForegroundColor DarkGray

    # The same named volume the Linux stage uses: each preset writes to its own
    # subdirectory of it, so the vendored fetches and the ccache-less object
    # trees all survive between runs and no stage disturbs another.
    docker run --name $shippingContainer `
        -v "${repo}:/repo" `
        -v "${buildVolume}:/build" `
        $script:tier2Image bash scripts/gates/shipping-build.sh
    if ($LASTEXITCODE -ne 0) { throw "the shipping, player or editor profile failed to build" }
}

# **The three profiles a WINDOWS release ships, on the compiler that ships them**.
#
# The `shipping` stage above builds `shipping`, `player` and `editor` -- on
# Tier-2, deliberately, because Clang with warnings-as-errors is the stricter
# reader of the `#if`s only those profiles take. What it cannot read is MSVC.
# And MSVC is what the Windows release is compiled with, so the three artifacts
# a person actually downloads were built by no gate on any machine: `ludwerk
# build` packages `win-msvc-player`, a released editor archive is
# `win-msvc-editor`, and neither had ever been compiled outside a release.
#
# The failure that hides here is specific rather than hypothetical. These
# profiles differ from `dev` by what `ENG_LUAU_COMPILER` and `ENG_DEBUG_UI`
# gate, so what rots in them is code inside an `#if` -- and a preprocessor
# branch that one compiler takes and another does not is precisely where the
# two disagree. This repository has its own instance: an unknown number of
# commits where the shipping profile did not compile at all.
#
# Opt-in for the same reason as `asan`: three Release configures and three
# builds of the vendored tree is minutes, not seconds, and the standing gate is
# something a person runs before every push. It belongs before a RELEASE, and here for the afternoon
# somebody has touched a `#if ENG_DEBUG_UI`.
#
# It runs the same `shipping-build.sh` the Tier-2 stage does, with the three
# preset names passed in -- the script already takes them that way, and one
# implementation of "which profiles and which target" is the rule. The `.cmd` around it exists only to supply a Developer Shell, which the
# Ninja presets require and Git Bash does not have.
Invoke-Stage 'winprofiles' {
    if (-not $Only) {
        Write-Host '[gate] winprofiles: skipped (opt-in; run scripts/localgate.ps1 -Only winprofiles)' -ForegroundColor DarkGray
        $script:stageSkipped = $true
        return
    }

    $vcvars = Get-DeveloperShellEnv
    $bash = Get-BashPath
    $script = @"
chcp 65001 >nul
call "$vcvars" >nul || exit /b 1
set ENG_SHIPPING_PRESET=win-msvc-shipping
set ENG_PLAYER_PRESET=win-msvc-player
set ENG_EDITOR_PRESET=win-msvc-editor
"$bash" scripts/gates/shipping-build.sh || exit /b 1
"@
    $temp = Join-Path $env:TEMP "engine-winprofiles-$PID.cmd"
    Set-Content -Path $temp -Value $script -Encoding ascii
    try {
        & cmd.exe /c $temp
        if ($LASTEXITCODE -ne 0) {
            throw "the shipping, player or editor profile failed to build on MSVC"
        }
    } finally {
        Remove-Item $temp -ErrorAction SilentlyContinue
    }
}

# **The real-image golden suite, which two documents promise and neither had**.
# `architecture.md` §9: "a small real-image golden suite (lavapipe on
# Linux, WARP/D3D12 on Windows) runs nightly, non-blocking". `roadmap.md` says
# the same. What existed was one recorded PNG that nothing compared, with a
# README explaining that a golden spanning a discrete GPU and a software
# rasterizer cannot see a real change any more.
#
# It does not span them. lavapipe renders a scene to the same bytes twice --
# measured at zero differing pixels, tolerance zero, before the goldens were
# recorded -- so these compare EXACTLY and a single changed pixel is a change.
#
# Opt-in and non-blocking, which is what both documents say: a Mesa upgrade in
# the Tier-2 image moves every pixel of every one of these, and that must not
# redden the gate somebody runs before a push. The nightly job runs the same
# script.
Invoke-Stage 'lavapipe' {
    # Asked for by name, or by a plan of what a change affects (ADR 0249).
    if (-not $Only -and -not ($script:affectedPlan -and $script:affectedPlan.stages.lavapipe.run)) {
        Write-Host '[gate] lavapipe: skipped (opt-in; run scripts/localgate.ps1 -Only lavapipe)' -ForegroundColor DarkGray
        $script:stageSkipped = $true
        return
    }
    Use-Tier2Image
    docker volume create $buildVolume | Out-Null

    $existing = docker ps -aq --filter "name=^${lavapipeContainer}$"
    if ($existing) { docker rm -f $lavapipeContainer | Out-Null }

    $arguments = @('bash', 'scripts/gates/lavapipe-goldens.sh')
    if ($Record) { $arguments += '--record' }

    docker run --name $lavapipeContainer `
        -v "${repo}:/repo" `
        -v "${buildVolume}:/build" `
        $script:tier2Image @arguments
    if ($LASTEXITCODE -ne 0) { throw "the lavapipe goldens did not match" }
}

# **The game's Nakama** (ADR 0242): a real Nakama and its database in
# containers, both pinned by digest, the integration's module mounted into it;
# then the engine's own build against it -- players signed in to Nakama, a
# session turned into a ticket at the front door, a thousand trades kept in
# Nakama's storage, and a request sent twice done once. Opt-in: it pulls two
# images and starts two containers, which a gate run every push should not.
# Run with the Windows build there (`-Only windows` first).
#
# **The images are a test's, never the engine's**: pulled here, run here, and
# shipped nowhere -- Nakama is Apache-2.0, PostgreSQL its own permissive
# license -- each pinned by digest (R5). The containers and their network are
# named by this run's process, never as the gate's own (`engine-tier2-*`,
# `engine-lavapipe-gate`), and removed whatever happens.
# The block of test ports the machine gave this checkout
# (`cmake/engine_test_ports.cmake`), or 0 when it has none yet.
function Get-TestPortBlock {
    $list = Join-Path $env:LOCALAPPDATA "$BrandName\test-port-blocks.txt"
    if (-not (Test-Path $list)) { return 0 }
    $here = [System.IO.Path]::GetFullPath($repo).TrimEnd([char]92, [char]47).Replace([char]92, [char]47).ToLowerInvariant()
    foreach ($line in Get-Content $list) {
        if ($line -match '^([0-5]) (.+)$' -and
            $Matches[2].Trim().TrimEnd([char]47).ToLowerInvariant() -eq $here) {
            return [int]$Matches[1]
        }
    }
    return 0
}
$NakamaImage = 'heroiclabs/nakama:3.41.0@sha256:ca9f3fe65c90f56860fe5d7cd024868a395cb1a001abb9912bf87ef47d605349'
$NakamaDatabase = 'postgres:16.10-alpine@sha256:029660641a0cfc575b14f336ba448fb8a75fd595d42e1fa316b9fb4378742297'
Invoke-Stage 'nakama' {
    if (-not $Only) {
        Write-Host '[gate] nakama: skipped (opt-in; run scripts/localgate.ps1 -Only nakama)' -ForegroundColor DarkGray
        $script:stageSkipped = $true
        return
    }
    if (-not $env:ENG_BUILD_ROOT) {
        $env:ENG_BUILD_ROOT = Join-Path $env:LOCALAPPDATA "$BrandName\build"
    }
    $engineHost = Join-Path $env:ENG_BUILD_ROOT 'win-msvc-dev\engine\app\engine-host.exe'
    if (-not (Test-Path $engineHost)) { throw "no engine-host at ${engineHost}: run -Only windows first" }
    # The ports this run takes on the host are in this checkout's block of
    # test ports, so two checkouts run this stage at once.
    $shift = (Get-TestPortBlock) * 1000
    $nakamaPort = 47350 - $shift
    $gamePort = 47896 - $shift
    Write-Host "[gate] nakama: test port block $($shift / 1000), Nakama on $nakamaPort, the game on $gamePort" -ForegroundColor DarkGray
    $network = "engine-nakama-$PID"
    $database = "engine-nakama-db-$PID"
    $server = "engine-nakama-$PID"
    $key = 'engine-test-http-key'
    $address = "postgres:localdb@${database}:5432/nakama"
    try {
        docker network create $network | Out-Null
        docker run -d --name $database --network $network -e POSTGRES_PASSWORD=localdb -e POSTGRES_DB=nakama `
            $NakamaDatabase | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Nakama's database did not start" }
        # The database is migrated once it answers; then Nakama, its module
        # loaded from the folder mounted where it looks.
        $start = "until /nakama/nakama migrate up --database.address $address; do sleep 1; done; " +
            "exec /nakama/nakama --name engine --database.address $address --runtime.http_key $key " +
            "--socket.server_key defaultkey"
        docker run -d --name $server --network $network -p ${nakamaPort}:7350 `
            -v "${repo}/integrations/nakama/modules:/nakama/data/modules" `
            --entrypoint /bin/sh $NakamaImage -ec $start | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Nakama did not start" }
        $ready = $false
        foreach ($try in 1..120) {
            try {
                $answer = Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 "http://127.0.0.1:${nakamaPort}/healthcheck"
                if ($answer.StatusCode -eq 200) { $ready = $true; break }
            }
            catch {}
            Start-Sleep -Seconds 1
        }
        if (-not $ready) {
            docker logs $server
            throw "Nakama did not answer its health check in two minutes"
        }
        # CMake as the other Windows stages find it: in the Developer Shell.
        $vcvars = Get-DeveloperShellEnv
        $driver = @"
chcp 65001 >nul
call "$vcvars" >nul || exit /b 1
set ENG_NAKAMA_HTTP_KEY=$key
cmake "-DHOST=$engineHost" "-DPROJECT=$repo/tests/data/nakama" "-DWORKDIR=$env:ENG_BUILD_ROOT/win-msvc-dev/nakama-test" -DPORT=$gamePort "-DNAKAMA=http://127.0.0.1:$nakamaPort" "-DTWICE=$repo/tests/data/backend-twice/twice.luau" "-DSOURCE=$repo" -P "$repo/tests/data/run_data_nakama.cmake" || exit /b 1
"@
        $driverTemp = Join-Path $env:TEMP "engine-localgate-nakama-$PID.cmd"
        Set-Content -Path $driverTemp -Value $driver -Encoding ascii
        try {
            & cmd.exe /c $driverTemp
            $failed = $LASTEXITCODE -ne 0
        }
        finally {
            Remove-Item $driverTemp -ErrorAction SilentlyContinue
        }
        if ($failed) {
            docker logs --tail 80 $server
            throw "the game did not keep its data and its players in Nakama"
        }
    }
    finally {
        docker rm -f $server $database 2>&1 | Out-Null
        docker network rm $network 2>&1 | Out-Null
    }
}

# **The engine cross-built for Android** (arm64, the pinned NDK), which only the
# nightly did until a surface shader's `std::from_chars(float)` -- a function
# the NDK's standard library does not have -- reached `main` and was found the
# next morning. Built the way the nightly's `Triangle APK` job builds it, and
# part of every run on a machine that has the NDK: `scripts/install-android.ps1`
# installs it. Without it, a full run says so and goes on; `-Only android` fails.
Invoke-Stage 'android' {
    $ndkVersion = (Select-String -Path 'cmake/toolchains/android.cmake' `
            -Pattern '^set\(ENG_ANDROID_NDK_VERSION "([^"]+)"').Matches[0].Groups[1].Value
    $sdkRoot = if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } elseif ($env:ANDROID_HOME) { $env:ANDROID_HOME } else {
        [Environment]::GetEnvironmentVariable('ANDROID_SDK_ROOT', 'User') }
    if (-not $sdkRoot -or -not (Test-Path (Join-Path $sdkRoot "ndk\$ndkVersion"))) {
        if ($Only) { throw "Android NDK $ndkVersion is not installed; run scripts/install-android.ps1" }
        Write-Host "[gate] android: skipped (no NDK $ndkVersion; scripts/install-android.ps1 installs it)" -ForegroundColor DarkGray
        $script:stageSkipped = $true
        return
    }

    $vcvars = Get-DeveloperShellEnv
    if (-not $env:ENG_BUILD_ROOT) {
        $env:ENG_BUILD_ROOT = Join-Path $env:LOCALAPPDATA "$BrandName\build"
    }
    $buildDir = Join-Path $env:ENG_BUILD_ROOT 'android-arm64'
    # And the whole player host, what a game on a phone is -- which needs the
    # Windows stage's `engine_luauembed`, a tool that runs on this machine.
    $playerDir = Join-Path $env:ENG_BUILD_ROOT 'android-arm64-player'
    $luauembed = Join-Path $env:ENG_BUILD_ROOT 'win-msvc-dev\engine\script\engine_luauembed.exe'
    if (-not (Test-Path $luauembed)) { throw "the android stage needs the windows stage's engine_luauembed; run it first" }
    # The developer shell for CMake and Ninja; the NDK's toolchain file picks
    # the compilers. chcp 65001 so that ninja records header dependencies.
    $script = @"
chcp 65001 >nul
call "$vcvars" >nul || exit /b 1
set ANDROID_SDK_ROOT=$sdkRoot
set ANDROID_HOME=$sdkRoot
cmake -S . -B "$buildDir" -G Ninja -DCMAKE_TOOLCHAIN_FILE="%CD%/cmake/toolchains/android.cmake" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENG_PROFILE=dev -DENG_BUILD_TESTS=OFF >nul || exit /b 1
cmake --build "$buildDir" --target engine_triangle_sample || exit /b 1
cmake -S . -B "$playerDir" -G Ninja -DCMAKE_TOOLCHAIN_FILE="%CD%/cmake/toolchains/android.cmake" -DCMAKE_BUILD_TYPE=Release -DENG_PROFILE=player -DENG_ENABLE_REPLICATION=ON -DENG_BUILD_TESTS=OFF -DENG_HOST_LUAUEMBED="$luauembed" >nul || exit /b 1
cmake --build "$playerDir" --target engine_host || exit /b 1
"@
    $temp = Join-Path $env:TEMP "engine-android-$PID.cmd"
    Set-Content -Path $temp -Value $script -Encoding ascii
    try {
        & cmd.exe /c $temp
        if ($LASTEXITCODE -ne 0) { throw "the engine does not build for Android arm64" }
    } finally {
        Remove-Item $temp -ErrorAction SilentlyContinue
    }

    # **The Android export, debug and release** (ADR 0104 stages 2-3): the
    # player just built, staged by `ludwerk build --target=android`, assembled by
    # Gradle and verified by `apksigner` -- once with this machine's debug key,
    # once with a release key `ludwerk keystore new` makes.
    $env:ANDROID_SDK_ROOT = $sdkRoot
    & lute test tests/android
    if ($LASTEXITCODE -ne 0) { throw "the Android export failed" }
}

Pop-Location

Write-Host ""
if (-not $Stages) { Write-ContainerDone }
Write-Host "=== local gate ===" -ForegroundColor Cyan
$script:results | ForEach-Object { Write-Host $_ }

if ($script:failures.Count -gt 0) {
    Write-Host ""
    Write-Host "FAILED: $($script:failures -join ', ')" -ForegroundColor Red
    exit 1
}

Write-Host ""
if ($SkipLinux) {
    # Named rather than folded into a generic "partial", because this is the one
    # skip that hides a whole compiler's diagnostics.
    Write-Host "green, but the Linux tier did not run -- Clang has not seen this change" -ForegroundColor Yellow
} elseif ($Only -or $Stages) {
    Write-Host "green (partial run -- macOS is Tier-3 and only CI can build it)" -ForegroundColor Yellow
} else {
    Write-Host "green (macOS is Tier-3 and only CI can build it)" -ForegroundColor Green
}
exit 0
