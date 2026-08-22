#!/bin/sh
# #602: runs every fuzz_*.c harness in this directory for a bounded time budget,
# coverage-guided (libFuzzer), collecting any crash into crashes/<name>/.
#
# Two corpus directories per target, on purpose:
#   corpus/<name>/        hand-authored seed_*/regression_*.bin -- committed to git,
#                         small, read on every run but never written to by libFuzzer
#                         (passed as an EXTRA seed dir, not the first argument).
#   corpus_grown/<name>/  the live, coverage-guided corpus libFuzzer discovers and
#                         grows at runtime -- can reach thousands of files, gitignored.
#
# Usage: ./run_fuzzers.sh [seconds_per_target] [target ...]
#   ./run_fuzzers.sh                # every target, 120s each (CI-sized smoke run)
#   ./run_fuzzers.sh 1800           # every target, 30 minutes each
#   ./run_fuzzers.sh 1800 fuzz_ahci # just one target, 30 minutes
#
# Exit status is nonzero if ANY target found a crash (an artifact file appeared under
# crashes/<name>/) -- a CI job can gate on this directly.
set -u
cd "$(dirname "$0")"

SECONDS_PER_TARGET="${1:-120}"
shift 2>/dev/null || true

if [ "$#" -gt 0 ]; then
    targets="$*"
else
    targets=""
    for f in fuzz_*.c; do
        name="${f%.c}"
        [ -x "$name" ] && targets="$targets $name"
    done
fi

mkdir -p crashes
overall_rc=0

for t in $targets; do
    name="${t#fuzz_}"
    seed_dir="corpus/$name"
    grown_dir="corpus_grown/$name"
    crash_dir="crashes/$name"
    mkdir -p "$seed_dir" "$grown_dir" "$crash_dir"
    echo "=== $t: running for ${SECONDS_PER_TARGET}s (seeds: $seed_dir, grown: $grown_dir) ==="
    ./"$t" -max_total_time="$SECONDS_PER_TARGET" -artifact_prefix="$crash_dir/" \
        -print_final_stats=1 "$grown_dir" "$seed_dir" \
        > "crashes/$name.log" 2>&1
    rc=$?
    tail -n 20 "crashes/$name.log"
    if [ "$rc" -ne 0 ]; then
        echo "*** $t EXITED $rc -- check $crash_dir/ and crashes/$name.log ***"
        overall_rc=1
    fi
    echo
done

exit $overall_rc
