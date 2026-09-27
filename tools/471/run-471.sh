#!/bin/bash
# SMP-15 (#471) leg: tests/micro/suite-471.cfg under QEMU -smp 2 -- one pool core shared by
# timekeep (halts between 100 Hz PIT ticks), a hog that never yields and a worker that never halts.
# PASS: timekeep counts 100 ticks per host RTC second within 1% across 30 s, the worker finishes,
# and the VECHIST injected/requested gap for timekeep's IRQ0 stays under 1% of the requests.
set -u
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
cd "$(git rev-parse --show-toplevel)"
export LC_ALL=C
LOG=disk-images/run-suite-suite-471.log
killall -9 "$(basename "$QEMU")" 2>/dev/null; sleep 1
SMP=2 SECS="${SECS:-150}" tools/micro/run-micro.sh --suite tests/micro/suite-471.cfg >/dev/null 2>&1
rc=0
grep -a "micro/timekeep:" "$LOG" | tail -2
grep -aq "MICRO PASS: timekeep" "$LOG" || { echo "FAIL: timekeep"; rc=1; }
grep -aq "micro/sharemem: 30 rounds written and verified" "$LOG" || { echo "FAIL: the worker did not finish"; rc=1; }
v=$(grep -a "fw-1 VECHIST vm0:" "$LOG" | tail -1)
echo "$v"
inj=$(echo "$v" | sed -n 's/.*0x20=\([0-9]*\)\/\([0-9]*\).*/\1/p')
req=$(echo "$v" | sed -n 's/.*0x20=\([0-9]*\)\/\([0-9]*\).*/\2/p')
if [ -n "$inj" ] && [ -n "$req" ] && [ "$req" -gt 0 ]; then
  gap=$((req - inj)); echo "IRQ0 injected $inj of $req requested (gap $gap)"
  [ $((gap * 100)) -le "$req" ] || { echo "FAIL: VECHIST gap above 1%"; rc=1; }
else
  echo "FAIL: no VECHIST line for vm0 IRQ0"; rc=1
fi
grep -a "fw-1 SCHED core0: vm[0-9]* vCPU" "$LOG" | tail -3
[ "$rc" -eq 0 ] && echo "PASS: #471 interrupts held across switches, timekeeping within 1%"
exit "$rc"
