#!/bin/bash
# SMP-21 (#477): the scheduler's own numbers must check out against the configuration.
# Runs tests/micro/suite-477.cfg (three hogs that never yield, one core, 2000 us slice) and
# checks that:
#   switches per second ~= 1 / slice            (within 10%)
#   each tenant's steal ~= 2 x its run time     (within 15%, three tenants round-robin)
set -u
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
cd "$(git rev-parse --show-toplevel)"
export LC_ALL=C
LOG=disk-images/run-suite-suite-477.log
killall -9 "$(basename "$QEMU")" 2>/dev/null; sleep 1
SMP=2 SECS="${SECS:-150}" tools/micro/run-micro.sh --suite tests/micro/suite-477.cfg >/dev/null 2>&1
rc=0
# Two consecutive 5 s reports -> switches/s.
s=$(grep -a "fw-1 SCHED core0: switches=" "$LOG" | tail -2 | sed -n 's/.*switches=\([0-9]*\) .*/\1/p' | tr '\n' ' ')
set -- $s
if [ $# -eq 2 ]; then
  rate=$(( ($2 - $1) / 5 )); echo "switches/s = $rate (slice 2000 us -> 500/s)"
  [ "$rate" -ge 450 ] && [ "$rate" -le 550 ] || { echo "FAIL: switch rate off the slice"; rc=1; }
else echo "FAIL: fewer than two SCHED reports"; rc=1; fi
grep -a "fw-1 SCHED core0: vm[0-9]* vCPU" "$LOG" | tail -3
for v in $(grep -a "fw-1 SCHED core0: vm[0-9]* vCPU" "$LOG" | tail -3 | sed -n 's/.*run=\([0-9]*\)ms steal=\([0-9]*\)ms.*/\1:\2/p'); do
  run=${v%%:*}; st=${v##*:}
  [ "$run" -gt 0 ] || { echo "FAIL: a hog never ran"; rc=1; continue; }
  ratio=$(( st * 100 / run )); echo "hog steal/run = ${ratio}% (expect ~200%)"
  [ "$ratio" -ge 170 ] && [ "$ratio" -le 230 ] || { echo "FAIL: steal does not match 3-way round robin"; rc=1; }
done
grep -a "fw-1 SCHED core0: runq" "$LOG" | tail -1
[ "$rc" -eq 0 ] && echo "PASS: #477 steal and switch counts agree with the slice and tenant count"
exit "$rc"
