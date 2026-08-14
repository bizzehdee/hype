#!/bin/sh
# #436: run the rig N times and report the outcome of each.
#
# One run cannot justify a conclusion here. The failure is non-deterministic:
# identical builds have produced probe-hit counts of 461, 170 and 50, so a
# single green run proves nothing and a single red run names nothing. Every
# claim about this bug should quote a distribution.
#
# Each run reports separately:
#   banner   - did hype itself start (a run that never banners is the #371
#              one-in-four no-boot, NOT a guest failure, and must not be scored
#              as one)
#   atapi    - how far the guest read its media before stopping
#   bugcheck - Windows' own KiBugCheckData, when the probe build is in use
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
N=${N:-3}
SECS=${SECS:-240}
i=1
while [ "$i" -le "$N" ]; do
    LOG="$REPO/rig/dist-$i.log" SECS="$SECS" sh "$HERE/run-win.sh" >/dev/null 2>&1 || true
    L="$REPO/rig/dist-$i.log"
    banner=$(LC_ALL=C grep -ac "hype: build" "$L" 2>/dev/null || echo 0)
    cmds=$(LC_ALL=C grep -aoE "cmds=[0-9]+" "$L" 2>/dev/null | tail -1 || true)
    bug=$(LC_ALL=C grep -aoE "BUGCHECK[^[]*" "$L" 2>/dev/null | tail -1 || true)
    printf 'run %d: banner=%s %s %s\n' "$i" "$banner" "${cmds:-cmds=?}" "${bug:-}"
    i=$(( i + 1 ))
done
