#!/bin/sh
# #597: run the ON-STICK battery code (hype_fat32_selftest_run, core/fat32_selftest.c) on this
# workstation, against a real mkfs.vfat image via fs_ops, and judge it with fsck.vfat. This proves
# the exact code hype runs on the stick -- the battery loop, the fs_ops mount, the self-check --
# before a stick is cut. run-fat32-e2e.sh is the lower-level twin that drives fat_write_fs directly.
#
# fs_ops's driver registry references every filesystem backend, so linking fs_ops.c drags the whole
# core tree; the networking/USB/halt modules pull device symbols that have no host build, so they
# are excluded and the two symbols the FAT path might still reach are stubbed. None of the excluded
# code is on the FAT32 write path.
set -eu
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
OUT=${OUT:-"$ROOT/disk-images/fat32-e2e"}
SIZE=${SIZE:-4G}
SPC=${SPC:-64}   # 32 KiB clusters -- the stick's geometry

for t in mkfs.vfat fsck.vfat; do
    command -v "$t" >/dev/null 2>&1 || { echo "MISSING TOOL: $t (need dosfstools)"; exit 2; }
done
mkdir -p "$OUT"

libs=""
for f in "$ROOT"/core/*.c; do
    case "$f" in
        *_hw.c|*/blk_usb.c|*/xhci.c|*/usb_msc.c|*/usb_hid.c|*/e1000.c|*/e1000_dev_ring.c|\
        */virtio_net_ring.c|*/arp.c|*/nat.c|*/guest_nic.c|*/l2switch.c|*/halt.c) continue ;;
    esac
    libs="$libs $f"
done
cat > "$OUT/stubs.c" <<'EOF'
#include <stdio.h>
void hype_fatal(const char *msg){ fprintf(stderr, "hype_fatal: %s\n", msg?msg:""); }
void hype_serial_putc(char c){ (void)c; }
EOF
cc -O1 -g -fno-builtin -I"$ROOT" -o "$OUT/selftest_host" selftest_host.c "$OUT/stubs.c" $libs

img="$OUT/selftest-host.img"
rm -f "$img"
truncate -s "$SIZE" "$img"
mkfs.vfat -F 32 -s "$SPC" -n HYPE597 "$img" >/dev/null

echo "=== hype_fat32_selftest_run (the ON-STICK code) via fs_ops ==="
rc=0
"$OUT/selftest_host" "$img" || rc=$?

echo "=== fsck.vfat -n ==="
fsck.vfat -n "$img" > "$OUT/fsck-selftest.txt" 2>&1 || true
if grep -qiE "cluster chain|beyond EOF|Free cluster summary wrong|orphan|lost cluster|Bad cluster|allocation size" "$OUT/fsck-selftest.txt"; then
    echo "  fsck.vfat: STRUCTURAL DAMAGE"; grep -vE '^$' "$OUT/fsck-selftest.txt" | sed 's/^/    /'; rc=1
else
    echo "  fsck.vfat: CLEAN"; grep -vE '^$' "$OUT/fsck-selftest.txt" | sed 's/^/    /'
fi

if [ "$rc" -eq 0 ]; then
    echo "PASS: the on-stick FAT32 battery produced a clean volume on the host"
else
    echo "FAIL: the on-stick FAT32 battery failed on the host -- see above"
fi
exit "$rc"
