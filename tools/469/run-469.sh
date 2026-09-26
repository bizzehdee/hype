#!/bin/bash
# SMP-13 (#469) legs under QEMU (-smp 2: the BSP keeps core 0, so the shared pool is ONE core).
#   A: two VMs of different trust groups alternate on the core (tests/micro/suite-469.cfg).
#   B: two vCPUs of ONE VM alternate on the core (tests/micro/suite-469-smp.cfg).
# Each guest writes and verifies its own seed's pattern over its RAM for 40 rounds; a missed
# NPT/EPT root or ASID/VPID swap shows as another seed's pattern. The SCHED lines prove the
# switching actually happened, and how often.
set -u
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
cd "$(git rev-parse --show-toplevel)"
export LC_ALL=C
rc=0
leg() {   # $1 = suite cfg, $2 = expected verdict count
  local out log
  killall -9 "$(basename "$QEMU")" 2>/dev/null; sleep 1
  out=$(SMP=2 SECS="${SECS:-150}" tools/micro/run-micro.sh --suite "$1" 2>&1)
  log="disk-images/run-suite-$(basename "$1" .cfg).log"
  echo "$out" | grep -E "^(PASS|FAIL|NOBOOT|INVALID)"
  echo "$out" | grep -q "^PASS   sharemem ($2 of $2 VM(s) reported)" || rc=1
  grep -a "fw-1 SCHED core0: switches" "$log" | tail -1
  grep -a "fw-1 SCHED core0: vm" "$log" | tail -"$2"
  grep -aq "no foreign pattern seen" "$log" || rc=1
}
echo "=== A: two VMs, one core ==="
leg tests/micro/suite-469.cfg 2
echo "=== B: one VM, two vCPUs, one core ==="
leg tests/micro/suite-469-smp.cfg 1
[ "$rc" -eq 0 ] && echo "PASS: #469 both legs"
exit "$rc"
