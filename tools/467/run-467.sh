#!/bin/bash
# SMP-11 (#467) leg. One boot, three VMs (tools/467/hype.cfg). QMP types `config alpha` and
# `config beta` at the dashboard. Checks: both new keys shown with the right (set)/(default)
# tag, and gamma (cpu_mode = shared + cpu_set) refused by admission (plan.md decision 84).
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
cd "$(git rev-parse --show-toplevel)"
killall -9 "$(basename "$QEMU")" 2>/dev/null || true
sleep 1
KEYS=$(cat tools/467/sendkeys.txt)
HYPE_CFG=tools/467/hype.cfg SENDKEYS="$KEYS" \
    tools/run-guest.sh disk-images/alpine-hype-dbg.iso 467-cpumode "${TIMEOUT:-180}" || true
LOG=disk-images/run-467-cpumode.log
export LC_ALL=C

rc=0
want() {
    if grep -aqF -- "$1" "$LOG"; then echo "ok:   $1"; else echo "FAIL: $1"; rc=1; fi
}
want "cfg[alpha]: cpu_mode = shared (set)"
want "cfg[alpha]: isolation_group = payroll (set)"
want "cfg[beta]: cpu_mode = dedicated (default)"
want "cfg[beta]: isolation_group = beta (default)"
want "adm: REFUSED -- vm2 sets cpu_mode = shared AND cpu_set"
want "adm: vm2 WILL NOT RUN"
[ "$rc" -eq 0 ] && echo "PASS: #467 keys shown with provenance; shared+cpu_set refused"
exit "$rc"
