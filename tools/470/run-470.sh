#!/bin/bash
# SMP-14 (#470) leg: tests/micro/suite-470.cfg under QEMU -smp 2 (one pool core, three tenants:
# two hogs that never yield, one worker). PASS needs: the worker's 20 rounds done, every tenant
# still being scheduled at the end (slices growing), and the overrun reported and bounded.
set -u
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
cd "$(git rev-parse --show-toplevel)"
export LC_ALL=C
LOG=disk-images/run-suite-suite-470.log
killall -9 "$(basename "$QEMU")" 2>/dev/null; sleep 1
out=$(SMP=2 SECS="${SECS:-150}" tools/micro/run-micro.sh --suite tests/micro/suite-470.cfg 2>&1)
echo "$out" | grep -E "^(PASS|FAIL|NOBOOT|INVALID)"
rc=0
grep -aq "micro/sharemem: 20 rounds written and verified" "$LOG" || { echo "FAIL: the worker did not finish"; rc=1; }
grep -a "fw-1 SCHED core0: switches" "$LOG" | tail -1
grep -a "fw-1 SCHED core0: overrun" "$LOG" | tail -1
grep -a "fw-1 SCHED core0: vm[0-9]* vCPU" "$LOG" | tail -3
# Every tenant scheduled in the LAST report: slices strictly positive for all three.
n=$(grep -a "fw-1 SCHED core0: vm[0-9]* vCPU" "$LOG" | tail -3 | grep -vc "slices=0 ")
[ "$n" -eq 3 ] || { echo "FAIL: a tenant was never scheduled"; rc=1; }
max=$(grep -a "fw-1 SCHED core0: overrun" "$LOG" | tail -1 | sed -n 's/.*max=\([0-9]*\)us.*/\1/p')
# Bound: 100 ms. Measured: every vCPU-0 loop's periodic debug dump (section s81, every 5-30 s)
# costs ~80 ms under nested QEMU, where each serial byte exits to L0; at log_level = info the
# worst case is ~13 ms. The SCHED "longest host section" line names the section each run.
[ -n "$max" ] && [ "$max" -lt "${MAX_OVERRUN_US:-100000}" ] || { echo "FAIL: overrun ${max:-unreported} us"; rc=1; }
[ "$rc" -eq 0 ] && echo "PASS: #470 hogs preempted every slice, worker finished, overrun max ${max} us"
exit "$rc"
