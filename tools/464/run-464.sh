#!/bin/sh
# #464: prove the FAT32 growth-failure invariant against real dosfstools.
#
# Builds a real FAT32 volume with mkfs.vfat, drives hype's own writer at it under two barrier
# policies, and asks fsck.vfat whether the result is a filesystem the operator's machine will
# accept. "dead" is the policy that matched the validation stick: the barrier fails from the
# second call onward and never recovers (#516 -- the stick rejects SYNCHRONIZE CACHE(10)).
#
# Both policies must leave a CLEAN volume. A failed write is allowed to lose the write; it is
# not allowed to leave a directory entry claiming more bytes than its cluster chain holds,
# which is what Linux reports as "fat_bmap_cluster: request beyond EOF" before applying
# errors=remount-ro.
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
OUT=${OUT:-/tmp/hype-464}
mkdir -p "$OUT"

cc -O1 -g -I"$ROOT" -o "$OUT/repro" fat32_fsck_repro.c \
    "$ROOT/core/fat_write_fs.c" "$ROOT/core/fat_write.c" "$ROOT/core/rtc.c"

rc=0
for policy in ok dead; do
    img="$OUT/fat32-$policy.img"
    rm -f "$img"
    # 64 MiB is the smallest size mkfs.vfat will lay out as FAT32 without argument.
    dd if=/dev/zero of="$img" bs=1M count=64 status=none
    mkfs.vfat -F 32 -n HYPE464 "$img" >/dev/null

    echo "--- barrier policy: $policy ---"
    "$OUT/repro" "$img" "$policy"

    fsck.vfat -n "$img" > "$OUT/fsck-$policy.txt" 2>&1 || true

    # Structural damage is the failure. A set dirty bit is not: growth_rollback deliberately
    # leaves the volume flagged when it could not complete, which is the difference between
    # "this write did not happen" and "run fsck before trusting this volume". Linux mounts a
    # dirty vfat volume read-write and warns; it applies errors=remount-ro to the structural
    # faults below.
    if grep -qE "cluster chain|beyond EOF|Free cluster summary wrong|orphan|lost cluster|Bad cluster" \
        "$OUT/fsck-$policy.txt"; then
        echo "fsck.vfat: STRUCTURAL DAMAGE"
        grep -vE "^$" "$OUT/fsck-$policy.txt" | sed 's/^/    /'
        rc=1
    elif grep -q "Dirty bit is set" "$OUT/fsck-$policy.txt"; then
        echo "fsck.vfat: structurally clean, dirty bit set (expected after a failed rollback)"
    else
        echo "fsck.vfat: CLEAN"
    fi
done

if [ "$rc" -eq 0 ]; then
    echo "PASS: both barrier policies left a clean FAT32 volume"
else
    echo "FAIL: a barrier policy left an inconsistent FAT32 volume"
fi
exit "$rc"
