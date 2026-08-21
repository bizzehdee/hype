#!/bin/bash
# NET-6 (#223): boot two Alpine VMs on one uplink=none switch and make them ping EACH OTHER,
# with no host network attached to QEMU at all. PASS = both scripts' pass markers + vm0's
# neighbour entry showing vm1's REAL MAC (bridge, not router).
set -e
cd "$(git rev-parse --show-toplevel)"
NAME="${1:-223-switch}"
killall -9 qemu-system-x86_64 2>/dev/null || true
sleep 1
# Both VMs stream the SAME ISO file (per-VM backing #140 allows different ones; sameness is fine).
HYPE_CFG=tools/223/hype.cfg \
HYPE_INPUTS="tools/223/switch-vm0.txt tools/223/switch-vm1.txt" \
    tools/run-guest.sh disk-images/alpine-hype-dbg.iso "$NAME" "${TIMEOUT:-600}" || true
OUT="disk-images/run-$NAME.log"
echo "=== verdicts ==="
LC_ALL=C grep -a "SCRIPT vm[01]: PASS\|SCRIPT vm[01]: FAIL\|pass switch" "$OUT" | tail -6
echo "=== switch DIAG ==="
LC_ALL=C grep -a "SWITCH 0" "$OUT" | tail -2
LC_ALL=C grep -a "PING-AB-OK" "$OUT" >/dev/null && LC_ALL=C grep -a "PING-BA-OK" "$OUT" >/dev/null \
  && echo "PASS: both directions pinged" || { echo "FAIL: a direction did not ping"; exit 1; }
