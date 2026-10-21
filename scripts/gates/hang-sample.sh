#!/usr/bin/env bash
# **The stacks of a test that hangs**, taken while it still runs.
#
#   scripts/gates/hang-sample.sh OUT_DIR [SECONDS]
#
# Runs until it is killed. Every 30 seconds it looks at the module test
# executables running (`engine_<module>_tests`), and each one that has run
# for SECONDS (600 unless given) has every thread's stack written to
# OUT_DIR/<name>-<pid>.txt, once: with `sample` on macOS, with gdb where
# there is one. ctest's own timeout (`ENG_MODULE_TEST_TIMEOUT`) stops it
# later; by then nothing of where it waited is left to read.
set -u

out="${1:?usage: hang-sample.sh OUT_DIR [SECONDS]}"
limit="${2:-600}"
mkdir -p "$out"

# `ps`'s elapsed time, [[dd-]hh:]mm:ss, in seconds.
seconds_of() {
    local t="$1" d=0 h=0 m=0 s=0 a b c
    if [[ "$t" == *-* ]]; then
        d="${t%%-*}"
        t="${t#*-}"
    fi
    IFS=: read -r a b c <<<"$t"
    if [ -n "${c:-}" ]; then
        h="$a" m="$b" s="$c"
    else
        m="$a" s="$b"
    fi
    echo $((10#$d * 86400 + 10#$h * 3600 + 10#$m * 60 + 10#$s))
}

while true; do
    while read -r pid etime comm; do
        name="$(basename "$comm")"
        case "$name" in
        engine_*_test*) ;;
        *) continue ;;
        esac
        [ -e "$out/.$pid" ] && continue
        [ "$(seconds_of "$etime")" -ge "$limit" ] || continue
        : >"$out/.$pid"
        echo "hang-sample: $name ($pid) has run $etime; taking its stacks"
        if command -v sample >/dev/null 2>&1; then
            sample "$pid" 5 -file "$out/$name-$pid.txt" >/dev/null 2>&1
        elif command -v gdb >/dev/null 2>&1; then
            gdb -p "$pid" -batch -ex "thread apply all bt" >"$out/$name-$pid.txt" 2>&1
        fi
    done < <(ps -axo pid=,etime=,comm= 2>/dev/null)
    sleep 30
done
