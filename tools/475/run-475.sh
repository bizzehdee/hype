#!/bin/bash
# SMP-19 (#475) leg: a two-node QEMU host (SRAT from -numa). PASS needs: hype reads 2 nodes from
# SRAT, reserves a node-1 pool, places vm1 on node 1 with its RAM carved from that pool (an
# address inside node 1's range), and both sharemem guests verify their RAM.
# The single-node no-op is covered by every other rig: QEMU without -numa has no SRAT.
set -u
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
cd "$(git rev-parse --show-toplevel)"
export LC_ALL=C
LOG=disk-images/run-suite-suite-475.log
killall -9 "$(basename "$QEMU")" 2>/dev/null; sleep 1
SMP='4,sockets=2,cores=2,threads=1' QEMU_MEM=8192 \
EXTRA_QEMU_ARGS='-object memory-backend-ram,id=m0,size=4G -object memory-backend-ram,id=m1,size=4G -numa node,nodeid=0,cpus=0-1,memdev=m0 -numa node,nodeid=1,cpus=2-3,memdev=m1' \
  SECS="${SECS:-150}" tools/micro/run-micro.sh --suite tests/micro/suite-475.cfg >/dev/null 2>&1
rc=0
grep -a "^numa:\|numa: " "$LOG" | head -12
grep -aq "numa: SRAT names 2 node(s)" "$LOG" || { echo "FAIL: SRAT not read as two nodes"; rc=1; }
grep -aq "numa: node 1 pool" "$LOG" || { echo "FAIL: no node-1 pool"; rc=1; }
grep -aq "numa: vm1 guest RAM .* on node 1" "$LOG" || { echo "FAIL: vm1 RAM not on node 1"; rc=1; }
n=$(grep -a "ttyS0" "$LOG" | grep -c "no foreign pattern seen"); [ "$n" -eq 2 ] || { echo "FAIL: $n of 2 guests verified RAM"; rc=1; }
[ "$rc" -eq 0 ] && echo "PASS: #475 vm1 runs and keeps its RAM on node 1"
exit "$rc"
