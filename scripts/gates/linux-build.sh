#!/usr/bin/env bash
# Tier-2 build and test. Runs inside the container scripts/docker/tier2.Dockerfile
# builds, and is the same sequence .github/workflows/ci.yml runs on ubuntu.
#
# Kept as a script rather than a docker `CMD` so the two callers -- the local
# gate and the workflow -- run identical commands. When they diverge, "it passes
# locally" stops meaning anything.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."

preset="${ENG_LINUX_PRESET:-linux-clang-dev}"

if [[ -z "${ENG_BUILD_ROOT:-}" ]]; then
    echo "linux-build: ENG_BUILD_ROOT is not set (rule R14: builds are out-of-tree)" >&2
    exit 1
fi

case "$ENG_BUILD_ROOT" in
"$PWD"*)
    echo "linux-build: ENG_BUILD_ROOT must be OUTSIDE the repository tree (rule R14)" >&2
    exit 1
    ;;
esac

# **The tests that measure time** (`LABELS perf`: `perf_budget`), three ways.
# `with`, the default and what the workflow runs: every test, as before.
# `without`: they are left out, to be run by themselves when nothing else of
# the local gate is running -- which runs six stages on one machine at once,
# and a wall-clock mean measured then is a measure of the other five. `only`:
# that run. Nothing is built: the tree is the one the run without them built.
perf="${ENG_GATE_PERF:-with}"
if [ "$perf" = "only" ]; then
    echo "== the tests that measure time, alone ($preset) =="
    ctest --preset "$preset" --output-on-failure --no-tests=error -L perf
    exit $?
fi

echo "== configure ($preset) =="
# **With the software pictures' tests left out, said and not assumed**.
# The lavapipe stage builds in this same tree and turns them on, and a cached
# option stays on: the next run of this stage listed thirteen tests the
# workflow's skip lists had never heard of, and failed its own check of them.
# They are that stage's; here they are off whatever ran before.
cmake --preset "$preset" -DENG_LAVAPIPE_GOLDENS=OFF

echo "== build =="
cmake --build --preset "$preset"

# rokit's shims are installed in the image but a non-interactive shell does not
# pick them up, the same reason luau-check.sh adds this path.
if [[ -d "$HOME/.rokit/bin" ]]; then
    PATH="$HOME/.rokit/bin:$PATH"
    export PATH
fi

echo "== test =="
# The screenshot gate skips here rather than failing: a container has no GPU,
# and `engine-host` reports that with its own exit code so CTest can tell the
# difference between "no driver" and "the picture changed". The capture gate
# does run, because it needs no GPU at all -- which is exactly why it, and not
# the image comparison, is the blocking render gate (architecture.md §9).
# `-LE gpu-golden` is what `engine/app/CMakeLists.txt` already says must happen
# to the pixel goldens: they are tied to the GPU that recorded them, and this
# tier is not it. That exclusion was written down and never applied here -- the
# ABSENCE of a Vulkan device was doing the work, so the tests skipped and nobody
# noticed. Installing lavapipe gave the tier a device and turned a skip into a
# red, which is how a comment that had never been executed came to light.
# **Under a virtual display**. This tier has a Vulkan device through
# lavapipe and had no screen at all, so every test that opens a window returned
# before it asserted -- and the one test that enters the editor shell reported a
# pass on a machine it had never run on. `xvfb-run -a` picks a free display
# number and tears the server down afterwards; tests that ask for a headless
# platform still get one, because that is a parameter they pass rather than
# something they infer from the environment.
#
# **Skips are asserted here too, and were not.** `localgate.ps1` fails on a
# skipped test and names it -- on the WINDOWS stage only, which is how
# `editor_shell` came to report Skipped on this tier inside a run that printed
# `ok linux`. The check belongs beside the ctest it is about, and in this file
# rather than in the PowerShell so that every caller of this gate gets it.
#
# Nothing is allowed to skip here: this tier has a device through lavapipe and a
# screen through Xvfb, so a skip means one of those stopped being found.
# **And every test that may skip on a machine with no graphics card is on the
# workflow's lists of tests allowed to** (`tools/repo/skiplists.luau`). Here a
# picture gate runs, so a new one missing from those lists is green in this
# stage and red on a hosted runner after the push -- which is how the branch
# went red twice in a week. Asked of CTest's own listing, before anything runs.
skipListing="$(mktemp --suffix=.json)"
ctest --preset "$preset" --show-only=json-v1 > "$skipListing"
lute tools/repo/skiplists.luau "$skipListing"
rm -f "$skipListing"

ctestLog="$(mktemp)"
trap 'rm -f "$ctestLog"' EXIT
set +e
# Every core the container has (ADR 0148): the tests that measure time run
# alone, and lavapipe draws on the CPU, so nothing else needs to take turns.
left="gpu-golden"
if [ "$perf" = "without" ]; then
    left="gpu-golden|perf"
    echo "(the tests that measure time are left to a run of their own: ENG_GATE_PERF=without)"
fi
xvfb-run -a ctest --preset "$preset" --output-on-failure --no-tests=error -j "$(nproc)" \
    --label-exclude "$left" 2>&1 | tee "$ctestLog"
ctestStatus=${PIPESTATUS[0]}
set -e
if [ "$ctestStatus" -ne 0 ]; then
    exit "$ctestStatus"
fi
bash "$(dirname "$0")/assert-no-skips.sh" "$ctestLog"

# The CLI's own path to the same suite, which the tooling loop's gate requires green "on
# both tiers". It runs the engine the build above produced -- ENG_BUILD_ROOT
# is how `ludwerk` finds it -- and turns the per-case report into TAP. Running it
# after ctest rather than instead of it is deliberate: ctest proves the engine,
# this proves the tool a developer actually types.
echo "== ludwerk test =="
bash scripts/ludwerk.sh test tests/conformance > /tmp/engine-test.tap
tail -n 3 /tmp/engine-test.tap

# The tooling loop's gate's first item. It starts a dev server, launches this build headless
# against it, mutates a file and waits for the reload to be confirmed over the
# WebSocket -- so it exercises inotify, the socket, the safe point and the world
# hash together, and it is the only thing that does.
echo "== hot reload (end to end) =="
lute test tests/hotreload
