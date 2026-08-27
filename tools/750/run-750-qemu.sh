#!/bin/bash
# #750: a guest soft lockup in smp_call_function_many_cond on a 4-vCPU guest.
#
# INTERMITTENT -- seen roughly one run in three -- so this rig runs the same boot N times
# and reports how many tripped. A single passing run says nothing about an intermittent
# fault, which is the whole reason it needs its own rig rather than a note on another one.
#
# The guest is tools/735's: Alpine, `vcpus = 2`, which is two whole physical cores whose
# SMT siblings come free (#564), so the guest sees FOUR logical CPUs. tools/525's 2-vCPU
# rig has never shown this.
#
# The signal is the guest's own watchdog line; hype's counters are captured alongside so a
# trip can be read against what hype was doing at the time.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
RUNS="${1:-3}"
SECS="${2:-420}"
S=disk-images/750
mkdir -p "$S"

trips=0
for r in $(seq 1 "$RUNS"); do
  echo "=== run $r of $RUNS ==="
  SCRATCH="$S/r$r" timeout $((SECS + 60)) ./tools/735/run-735-qemu.sh "$SECS" >"$S/out-$r.txt" 2>&1 || true
  L="$S/r$r/serial.txt"
  if [ ! -f "$L" ]; then echo "  no serial log -- run did not start"; continue; fi
  if grep -aq 'soft lockup' "$L"; then
    trips=$((trips + 1))
    echo "  TRIPPED:"
    grep -a 'soft lockup' "$L" | head -2 | sed 's/^/    /'
    grep -a -A3 'soft lockup' "$L" | grep -a 'RIP:' | head -1 | sed 's/^/    /'
  else
    echo "  clean"
  fi
  # What hype was doing, either way -- a clean run's numbers are the control.
  # #750: per-vCPU, all four. The aggregate hid the one vCPU that lost an IPI.
  for v in 0 1 2 3; do
    grep -a "VECSTAT vm0/$v:" "$L" | tail -1 | sed 's/^/    /'
    grep -a "INTDIAG vm0/$v:" "$L" | tail -1 | sed 's/^/    /'
  done
done

echo "=== verdict ==="
echo "soft lockups: $trips of $RUNS runs"
if [ "$trips" -eq 0 ]; then
  echo "NOT REPRODUCED in $RUNS runs. That is not the same as fixed -- the observed rate was"
  echo "about 1 in 3, so $RUNS clean runs leaves a real chance of having missed it."
  exit 0
fi
echo "REPRODUCED -- see the RIP above and the INTDIAG line from the same run"
exit 1
