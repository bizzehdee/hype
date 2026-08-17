#!/bin/sh
# #517: prove the exFAT growth-failure invariant against real exfatprogs.
#
# Builds a real exFAT volume with mkfs.exfat, drives hype's own writer at it under two failure
# policies, and asks fsck.exfat whether the result is a filesystem the host will accept. Both
# policies must leave a structurally sound volume: a failed write may lose the write, but it may
# never leave an entry set claiming more bytes than its cluster chain holds.
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
OUT=${OUT:-/tmp/hype-517}
mkdir -p "$OUT"

cc -O1 -g -I"$ROOT" -o "$OUT/repro" exfat_fsck_repro.c \
    "$ROOT/core/fat_exfat_fs.c" "$ROOT/core/fat_exfat.c" "$ROOT/core/rtc.c"

rc=0
for policy in ok dead; do
    img="$OUT/exfat-$policy.img"
    rm -f "$img"
    dd if=/dev/zero of="$img" bs=1M count=64 status=none
    mkfs.exfat -n HYPE517 "$img" >/dev/null 2>&1

    echo "--- write policy: $policy ---"
    "$OUT/repro" "$img" "$policy"

    fsck.exfat -n "$img" > "$OUT/fsck-$policy.txt" 2>&1 || true
    if grep -qiE "corrupt|invalid|mismatch|inconsistent|error" "$OUT/fsck-$policy.txt"; then
        echo "fsck.exfat: STRUCTURAL DAMAGE"
        sed 's/^/    /' "$OUT/fsck-$policy.txt"
        rc=1
    else
        echo "fsck.exfat: clean"
    fi
done

if [ "$rc" -eq 0 ]; then
    echo "PASS: both policies left a structurally sound exFAT volume"
else
    echo "FAIL: a policy left an inconsistent exFAT volume"
fi
exit "$rc"
