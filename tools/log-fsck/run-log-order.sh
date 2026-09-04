#!/bin/bash
# #809: check that a hype log's record order matches the order the records were produced,
# and report any produced bytes missing from the file.
#
#   tools/log-fsck/run-log-order.sh <log> [<log> ...]
#
# PASS EVERY LOG FROM ONE RUN TOGETHER -- the union is what is supposed to be complete:
#
#   tools/log-fsck/run-log-order.sh tools/hw-val-2026-08-25/logs/bootAMDL0-5/*.LOG
#
# \HYPE.LOG alone is opened with HYPE_LOG_SINK_HYPE, not _ALL, so guest console records live in
# the per-VM logs and a gap in the combined log by itself usually means nothing (#338 retired
# \HYPEFULL.LOG for exactly this reason). Checking it alone reported 4-5 KB "missing" on five
# AMD-laptop runs, none of which was real.
#
# Exits non-zero if ANY file has a backward jump. Gaps are reported and do not fail: a run that
# lost its tail has a gap by construction. Every conclusion drawn from a hype log assumes file
# order, and nothing checked it until boot AMD-L0 run 5 needed the question answered.
set -u
cd "$(git rev-parse --show-toplevel)"
BIN="${TMPDIR:-/tmp}/hype-log-order.$$"
cc -O2 -Wall -Wextra -o "$BIN" tools/log-fsck/log_order.c || exit 2
# ONE invocation with every path: the binary's union check is the whole point, and a loop that
# called it per file would silently reduce it to the single-file check this commit replaced.
rc=0
"$BIN" "$@" || rc=$?
rm -f "$BIN"
exit "$rc"
