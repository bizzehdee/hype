#!/bin/bash
# SMP-20 (#476): the plan.md 6g re-derivation, as repeatable QEMU tests.
#   1 a spinning guest cannot starve its co-tenants   -> tools/470/run-470.sh (reused)
#   2 a guest cannot defer its own preemption         -> tests/micro/suite-476-defer.cfg
#   3 a faulted guest is torn down alone              -> tests/micro/suite-476-fault.cfg
#   4 memory isolation under adversarial scheduling   -> tests/micro/suite-476-mem.cfg
#   5 worst-case scheduling latency, measured         -> every leg's wait_max, checked < bound
# A leg's bound for wait_max is (tenants on that core - 1) x slice plus 100 ms for one periodic
# debug dump (the #470 measurement: ~80 ms per dump under nested QEMU, section s81).
set -u
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
cd "$(git rev-parse --show-toplevel)"
export LC_ALL=C
rc=0
fail() { echo "FAIL: $*"; rc=1; }

run_suite() {   # $1 = smp, $2 = cfg
  killall -9 "$(basename "$QEMU")" 2>/dev/null; sleep 1
  SMP="$1" SECS="${SECS:-150}" tools/micro/run-micro.sh --suite "$2" >/dev/null 2>&1
  echo "disk-images/run-suite-$(basename "$2" .cfg).log"
}

# The largest wait_max (us) in the LAST report of each core of a log.
max_wait() { grep -a "fw-1 SCHED core.*wait_max=" "$1" | tail -"$2" | sed -n 's/.*wait_max=\([0-9]*\)us.*/\1/p' | sort -n | tail -1; }
# Every tenant's slices in the last report are > 0.
all_scheduled() { [ "$(grep -a "fw-1 SCHED core.*: vm[0-9]* vCPU" "$1" | tail -"$2" | grep -c "slices=0 ")" -eq 0 ]; }

echo "=== 1: spinning guests vs a worker (tools/470) ==="
tools/470/run-470.sh || fail "proof 1"
w=$(max_wait disk-images/run-suite-suite-470.log 3); echo "wait_max ${w}us (3 tenants, bound $((2*2000+100000))us)"
[ -n "$w" ] && [ "$w" -lt $((2*2000+100000)) ] || fail "proof 5 on leg 1: wait_max ${w:-none}"

echo "=== 2: seven ways to keep a core ==="
L=$(run_suite 3 tests/micro/suite-476-defer.cfg)
grep -aq "micro/sharemem: 15 rounds written and verified" "$L" || fail "proof 2: the worker did not finish"
all_scheduled "$L" 8 || fail "proof 2: a tenant was never scheduled"
grep -a "fw-1 SCHED core.*overrun" "$L" | tail -2
grep -a "fw-1 SCHED core.*: vm[0-9]* vCPU" "$L" | tail -8
w=$(max_wait "$L" 8); echo "wait_max ${w}us (4 tenants per core, bound $((3*2000+100000))us)"
[ -n "$w" ] && [ "$w" -lt $((3*2000+100000)) ] || fail "proof 5 on leg 2: wait_max ${w:-none}"

echo "=== 3: a faulted guest stops alone ==="
L=$(run_suite 2 tests/micro/suite-476-fault.cfg)
grep -aq "MICRO FAIL: faulter" "$L" || fail "proof 3: the faulter never ran"
grep -aq "micro/sharemem: 15 rounds written and verified" "$L" || fail "proof 3: the worker did not finish"
grep -aq "PANIC" "$L" && fail "proof 3: hype panicked"
grep -a "fw-1 SCHED core0: switches" "$L" | tail -1

echo "=== 4: memory isolation, adversarial ==="
L=$(run_suite 2 tests/micro/suite-476-mem.cfg)
n=$(grep -a "ttyS0" "$L" | grep -c "no foreign pattern seen"); [ "$n" -ge 2 ] || fail "proof 4: $n of 2 memory guests passed"
grep -a "fw-1 SCHED core0: flushes" "$L" | tail -1
grep -a "fw-1 SCHED core0: shared-tag" "$L" | tail -1

[ "$rc" -eq 0 ] && echo "PASS: #476 all five proofs hold under QEMU"
exit "$rc"
