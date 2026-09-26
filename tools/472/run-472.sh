#!/bin/bash
# SMP-16 (#472) leg: two boots of tools/472/hype.cfg.in.
#   A: default ratio 4.0 -> limit 8 sCPUs; beta+gamma ask 4 -> admitted.
#   B: ratio 1.5 -> limit 3; beta (2) fits, gamma crosses at 4 -> gamma alone refused.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
cd "$(git rev-parse --show-toplevel)"
export LC_ALL=C
D=disk-images/472
mkdir -p "$D"
rc=0
want() { if grep -aqF -- "$2" "$1"; then echo "ok:   $2"; else echo "FAIL: $2"; rc=1; fi; }
wont() { if grep -aqF -- "$2" "$1"; then echo "FAIL: unexpected: $2"; rc=1; else echo "ok:   absent: $2"; fi; }

leg() {
  sed "s/@RATIO@/$2/" tools/472/hype.cfg.in > "$D/hype-$1.cfg"
  killall -9 "$(basename "$QEMU")" 2>/dev/null || true; sleep 1
  HYPE_CFG="$D/hype-$1.cfg" SMP=4 tools/run-guest.sh disk-images/alpine-hype-dbg.iso "472-$1" "${TIMEOUT:-75}" >/dev/null 2>&1 || true
  echo "disk-images/run-472-$1.log"
}
A=$(leg a "")
echo "=== A: default ratio ==="
want "$A" "adm: tiers -- dedicated 1 core(s); shared pool 2 core(s) / 2 thread(s); 2 shared VM(s) asking 4 sCPU(s) of a 8 limit (ratio 4.00)"
wont "$A" "[#472 section 6i]"
B=$(leg b "shared_overcommit_ratio = 1.5")
echo "=== B: ratio 1.5 ==="
want "$B" "2 shared VM(s) asking 4 sCPU(s) of a 3 limit (ratio 1.50)"
want "$B" "adm: REFUSED -- the shared sCPUs pass shared_overcommit_ratio x pool threads, from vm2 on"
want "$B" "adm: vm2 WILL NOT RUN"
wont "$B" "adm: vm1 WILL NOT RUN"
[ "$rc" -eq 0 ] && echo "PASS: #472 tiers priced and refused as configured"
exit "$rc"
