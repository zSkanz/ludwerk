#!/usr/bin/env bash
# backoff.sh <command> [args...]
#
# Runs a command, and runs it again on failure, five times in all over about
# four minutes, waiting longer each time. For the downloads a CI job makes
# from GitHub before it builds anything: GitHub's own outage is not a red main.
# Its release downloads answered 500 for a minute on run 38570644428, and the
# job died before it built anything. `curl --retry` alone gave up after
# twenty-five seconds.
set -uo pipefail

wait=15
for attempt in 1 2 3 4 5; do
    if "$@"; then
        exit 0
    fi
    if [[ $attempt -lt 5 ]]; then
        echo "::warning::attempt $attempt of '$1' failed; trying again in ${wait}s"
        sleep "$wait"
        wait=$((wait * 2))
    fi
done
exit 1
