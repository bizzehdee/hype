#!/bin/bash
# SMP-18 (#474): the cross-group flush is issued exactly on cross-group switches and never within
# a group. Reads two runs of the #469 rig's suites:
#   tests/micro/suite-469.cfg      two VMs, two groups, one core  -> flushes == group_switches > 0
#   tests/micro/suite-469-smp.cfg  one VM, two vCPUs, one group   -> flushes == group_switches == 0
# and prints the startup capability line and the measured per-flush cost.
set -u
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
cd "$(git rev-parse --show-toplevel)"
export LC_ALL=C
rc=0
for s in suite-469 suite-469-smp; do
  killall -9 "$(basename "$QEMU")" 2>/dev/null; sleep 1
  SMP=2 SECS="${SECS:-150}" tools/micro/run-micro.sh --suite "tests/micro/$s.cfg" >/dev/null 2>&1
  L="disk-images/run-suite-$s.log"
  echo "=== $s ==="
  grep -a "adm: shared tier cross-group flush" "$L" | head -1
  line=$(grep -a "fw-1 SCHED core0: flushes=" "$L" | tail -1); echo "$line"
  f=$(echo "$line" | sed -n 's/.*flushes=\([0-9]*\) of group_switches=\([0-9]*\).*/\1/p')
  g=$(echo "$line" | sed -n 's/.*flushes=\([0-9]*\) of group_switches=\([0-9]*\).*/\2/p')
  [ -n "$f" ] && [ "$f" = "$g" ] || { echo "FAIL: flushes ($f) != group switches ($g)"; rc=1; }
  if [ "$s" = suite-469 ]; then [ "${f:-0}" -gt 0 ] || { echo "FAIL: no cross-group flush"; rc=1; }
  else [ "${f:-1}" -eq 0 ] || { echo "FAIL: a flush inside one group"; rc=1; }; fi
done
[ "$rc" -eq 0 ] && echo "PASS: #474 flush exactly on cross-group switches"
exit "$rc"
