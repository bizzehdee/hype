#!/bin/bash
# #809: check that a hype log's record order matches the order the records were produced,
# and report any produced bytes missing from the file.
#
#   tools/log-fsck/run-log-order.sh <log> [<log> ...]
#   tools/log-fsck/run-log-order.sh tools/hw-val-2026-08-25/logs/*/HYPE.LOG
#
# Exits non-zero if ANY file has a backward jump. Gaps are reported and do not fail: a filtered
# per-VM sink legitimately holds a subset of records, and a run that lost its tail has a gap by
# construction. Every conclusion drawn from a hype log assumes file order, and nothing checked it
# until boot AMD-L0 run 5 needed the question answered.
set -u
cd "$(git rev-parse --show-toplevel)"
BIN="${TMPDIR:-/tmp}/hype-log-order.$$"
cc -O2 -Wall -Wextra -o "$BIN" tools/log-fsck/log_order.c || exit 2
rc=0
for f in "$@"; do
    "$BIN" "$f" || rc=1
done
rm -f "$BIN"
exit "$rc"
