#!/bin/sh
# #597: end-to-end validation of hype's FAT32 writer against real dosfstools + mtools.
#
# Drives hype's own writer (core/fat_write_fs.c + core/fat_write.c) at a real mkfs.vfat volume
# with many small, medium and large files across every write path, then judges the result the way
# the operator's machine does:
#   - fsck.vfat -n must report the volume STRUCTURALLY CLEAN (this is the check that catches #596:
#     a chain hype can round-trip but Linux cannot);
#   - mtools mcopy reads every file back and it must be BYTE-EXACT to what hype wrote.
# The driver additionally self-checks every file through hype_fat32_read_at; a divergence between
# that and mtools is itself a bug.
#
# The image uses 32 KiB clusters (mkfs.vfat -s 64), the geometry of the validation stick, so the
# FAT-sector spanning and FSInfo churn match what real hardware exercises.
#
# Both barrier policies (ok, dead) must leave a clean volume. A set dirty bit under "dead" is not
# damage -- it means "run fsck before trusting this volume", not "this filesystem is broken".
set -eu
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
OUT=${OUT:-"$ROOT/disk-images/fat32-e2e"}
SIZE=${SIZE:-4G}       # sparse; only written clusters cost disk
SPC=${SPC:-64}         # sectors per cluster -> 32 KiB, matching the stick

for t in mkfs.vfat fsck.vfat mcopy; do
    command -v "$t" >/dev/null 2>&1 || { echo "MISSING TOOL: $t (need dosfstools + mtools)"; exit 2; }
done

mkdir -p "$OUT"
cc -O1 -g -fno-builtin -I"$ROOT" -o "$OUT/e2e" fat32_e2e.c \
    "$ROOT/core/fat_write_fs.c" "$ROOT/core/fat_write.c" "$ROOT/core/rtc.c"

# The full battery runs under "ok" (barriers succeed) -- the mode that matches the real #596
# scenario, where every write succeeded and a chain still corrupted. The barrier-FAILURE rollback
# invariant (a failed SYNCHRONIZE CACHE must never leave a chain shorter than the recorded size)
# is covered by tools/464, which arms the dead barrier around a single growth; failing every
# barrier here would abort the battery after one file and prove nothing broad.
rc=0
for policy in ${POLICIES:-ok}; do
    img="$OUT/fat32-$policy.img"
    expect="$OUT/expect-$policy"
    manifest="$OUT/manifest-$policy.txt"
    rm -rf "$img" "$expect" "$manifest"
    mkdir -p "$expect"
    truncate -s "$SIZE" "$img"
    mkfs.vfat -F 32 -s "$SPC" -n HYPE597 "$img" >/dev/null

    echo "=== barrier policy: $policy ==="
    if ! "$OUT/e2e" "$img" "$policy" "$expect" "$manifest"; then
        echo "  hype-side self-check FAILED under policy $policy"
        rc=1
    fi

    # 1) structural judgement by fsck.vfat
    fsck.vfat -n "$img" > "$OUT/fsck-$policy.txt" 2>&1 || true
    if grep -qiE "cluster chain|beyond EOF|Free cluster summary wrong|orphan|lost cluster|Bad cluster|allocated cluster|allocation size|Contains a free cluster" \
        "$OUT/fsck-$policy.txt"; then
        echo "  fsck.vfat: STRUCTURAL DAMAGE"
        grep -vE "^$" "$OUT/fsck-$policy.txt" | sed 's/^/    /'
        rc=1
    elif grep -qi "Dirty bit is set" "$OUT/fsck-$policy.txt"; then
        echo "  fsck.vfat: structurally clean (dirty bit set -- expected after a failed barrier)"
    else
        echo "  fsck.vfat: CLEAN"
    fi

    # 2) independent byte-exact read-back of every surviving file via mtools
    files=0; bad=0
    while read -r path size; do
        [ -n "$path" ] || continue
        files=$((files + 1))
        flat=$(printf '%s' "$path" | tr '/' '_')
        out="$OUT/mcopy.tmp"
        rm -f "$out"
        if ! mcopy -n -i "$img" "::$path" "$out" 2>/dev/null; then
            echo "  mcopy MISSING: $path"
            bad=$((bad + 1)); rc=1; continue
        fi
        if ! cmp -s "$out" "$expect/$flat"; then
            echo "  BYTE MISMATCH: $path (size $size)"
            bad=$((bad + 1)); rc=1
        fi
    done < "$manifest"
    echo "  mtools byte-exact: $((files - bad))/$files files verified"
done

rm -f "$OUT/mcopy.tmp"
if [ "$rc" -eq 0 ]; then
    echo "PASS: FAT32 writer produced a clean, byte-exact volume (policies: ${POLICIES:-ok})"
else
    echo "FAIL: FAT32 writer left an inconsistent or wrong volume -- see output above"
fi
exit "$rc"
