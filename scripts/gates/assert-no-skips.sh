#!/usr/bin/env bash
# A skipped test is a test that did not run, and CTest counts one as a pass.
#
# Six gates in this repository answer `ENG_TEST_SKIP` when there is no
# graphics device, and `editor_shell` answers it when there is no display. On a
# machine that has both, every one of them is supposed to execute -- so a skip
# means the device stopped being found, or a test stopped being able to reach
# it, and the run is green either way. That is the failure this exists for.
#
#   assert-no-skips.sh <ctest-log> [expected-skip ...]
#
# Named skips are ALLOWED and anything else is a failure. Passing no names means
# no skip is acceptable, which is the right setting for a machine with a device.
# The names are matched exactly against CTest's own summary lines.
#
# **An expected skip that did NOT skip is not a failure.** A runner that grows a
# GPU should not need this file edited before it can be green; the list is a
# ceiling on what may skip, not a claim about what will.
set -euo pipefail

log="${1:?usage: assert-no-skips.sh <ctest-log> [expected-skip ...]}"
shift || true

if [[ ! -f "$log" ]]; then
    echo "assert-no-skips: no such log: $log" >&2
    exit 2
fi

# CTest prints `  3/51 Test  #3: name .......***Skipped   0.01 sec`. The name is
# the field between the colon and the run of dots.
#
# Read with a loop rather than `mapfile`: macOS ships bash 3.2, which has no
# `mapfile`. This failed there with "command not found" the first time the
# macOS job stopped being allowed to fail.
skipped=()
while IFS= read -r name; do
    skipped+=("$name")
done < <(
    grep -oE '^[[:space:]]*[0-9]+/[0-9]+[[:space:]]+Test[[:space:]]+#[0-9]+:[[:space:]]+[^[:space:]]+[[:space:]]+\.+\*+Skipped' "$log" \
        | sed -E 's/.*#[0-9]+:[[:space:]]+([^[:space:]]+)[[:space:]]+\.+\*+Skipped/\1/' \
        | sort -u
)

# **Skipped because the device was REMOVED under the test**: not a
# skip to refuse, whoever's it is. Every test that draws carries the skip
# (`cmake/engine_device_tests.cmake`), the ones no list below names among
# them -- a list says who may skip for want of a device, and this is not a
# want: the runner had one and took it away. Found here, before the skips are
# judged, in CTest's own log of what every test printed
# (`ENG_CTEST_LAST_LOG`, or the newest `Testing/Temporary/LastTest.log` under
# `ENG_BUILD_ROOT`); with neither, nothing is known and nothing is allowed.
last="${ENG_CTEST_LAST_LOG:-}"
if [[ -z "$last" && -n "${ENG_BUILD_ROOT:-}" ]]; then
    last="$(ls -t "$ENG_BUILD_ROOT"/*/Testing/Temporary/LastTest.log 2>/dev/null | head -n 1 || true)"
fi
# What says so: a driver's own line, and the KEY of the host's sentence
# (`engine.info.device_taken_away`; every drawn test's host writes its
# messages with their keys). The key and not the words: this matched the
# sentence itself, the sentence was reworded, and a device really removed
# would have been a failure of whatever test it was removed under. And never
# a test's command line, which for three tests carries the key as an
# argument: they were counted as removed on every runner with no device.
taken='the graphics device was removed under the test|engine[.]info[.]device_taken_away'
removed=""
if [[ -n "$last" && -f "$last" ]]; then
    said="$(awk -v taken="$taken" '
        /^[0-9]+\/[0-9]+ Testing: / { name = $3 }
        /^Command: / { next }
        $0 ~ taken { if (name != "") print name }
    ' "$last" | sort -u | tr '\n' ' ' | sed 's/ *$//')"
    # **Only the ones CTest skipped.** One test prints the sentence on
    # purpose and passes -- `device_removed_survived`, which removes its own
    # device to see the host survive it, and whose command line carries the
    # sentence besides -- and it was counted here, every run, as a device
    # the runner had taken: a warning that is always there says nothing the
    # day it is true.
    for name in $said; do
        for one in "${skipped[@]:-}"; do
            if [[ "$name" == "$one" ]]; then
                removed="${removed:+$removed }$name"
            fi
        done
    done
fi

unexpected=()
for name in "${skipped[@]:-}"; do
    [[ -z "$name" ]] && continue
    allowed=0
    for gone in $removed; do
        if [[ "$name" == "$gone" ]]; then
            allowed=1
            break
        fi
    done
    for expected in "$@"; do
        if [[ "$name" == "$expected" ]]; then
            allowed=1
            break
        fi
    done
    if [[ $allowed -eq 0 ]]; then
        unexpected+=("$name")
    fi
done

if [[ ${#unexpected[@]} -gt 0 ]]; then
    echo "assert-no-skips: ${#unexpected[@]} test(s) SKIPPED that were not expected to: ${unexpected[*]}" >&2
    echo "  CTest counts a skip as a pass, so this run reported green having not run them." >&2
    echo "  On a machine with a graphics device and a display these are supposed to execute." >&2
    echo "  If this environment genuinely cannot run one, name it in the caller's expected list" >&2
    echo "  -- which is a decision somebody writes down, rather than a silence." >&2
    exit 1
fi

if [[ ${#skipped[@]} -gt 0 && -n "${skipped[0]}" ]]; then
    echo "assert-no-skips: ok -- ${#skipped[@]} expected skip(s): ${skipped[*]}"
else
    echo "assert-no-skips: ok -- nothing skipped"
fi

# **Skipped because the device was REMOVED under the test**: counted,
# by name, where a person reads a run -- its log, an annotation on it and its
# summary. A picture gate whose host lost its device says so and skips; a
# skip is quiet by nature, and a runner that removes its device every day
# would look exactly like one that never does. **The same test three runs
# running is a defect to write down, not weather.**
#
# CTest shows nothing of a test that skipped, so what each said is shown
# from the log found above: the block of each test that was skipped and said
# the sentence, and of no other.
if [[ -n "$last" && -f "$last" ]]; then
    if [[ -n "$removed" ]]; then
        count="$(echo "$removed" | wc -w | tr -d ' ')"
        echo "assert-no-skips: $count test(s) skipped because the graphics DEVICE WAS REMOVED under them: $removed"
        echo "  Not a pass and not a failure of theirs: the runner took the device away. Each printed what its"
        echo "  host said, with the crash report where one was written, into CTest's own log -- shown below."
        echo "  The same test three runs running is a defect to write down."
        echo "::warning title=Graphics device removed under a test::$count test(s) skipped: $removed"
        if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
            echo "**Graphics device removed under $count test(s):** $removed" >> "$GITHUB_STEP_SUMMARY"
        fi
        # What each said, crash report and all: the stack is what the second
        # sighting did not have.
        awk -v taken="$taken" -v names=" $removed " '
            /^[0-9]+\/[0-9]+ Testing: / {
                if (keep) printf "%s", block
                block = ""
                keep = 0
                mine = index(names, " " $3 " ") > 0
            }
            { block = block $0 "\n" }
            $0 ~ taken { if (mine) keep = 1 }
            END { if (keep) printf "%s", block }
        ' "$last"
    fi
fi
