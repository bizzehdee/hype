#!/bin/bash
# #506 bar: a guest writes into a hole of a sparse raw image on an ext volume. The write
# persists (verified after a FRESH mount, simulating a VM restart -- nothing carried over but
# the bytes on the medium), e2fsck -fn reports no errors after every step, and the file's
# allocated size on the host grows by roughly the amount written and no more.
#
# Real mke2fs, real e2fsck, real bytes -- no QEMU guest here. #505 already proved the
# guest-disk-attach/virtio-blk plumbing end-to-end; this exercises the NEW growth mechanism
# (hype_blk_image_sparse_t + ext's own jbd2-journaled hole-filling writer) against a real ext4
# volume, which is the actual risk surface #506 is about.
set -e
set -o pipefail
cd "$(git rev-parse --show-toplevel)"
S=$(mktemp -d)
trap 'rm -rf "$S"' EXIT
CC=${CC:-clang}

$CC -std=c11 -O1 -Wall -o "$S/sparse-harness" tools/506/sparse-harness.c \
    core/fs_ops.c core/ext.c core/ext2_alloc.c core/ext_jalloc.c core/ext_csum.c core/jbd2.c \
    core/ext_write.c core/ext_dirent.c core/ext_namespace.c core/ext2_namespace.c \
    core/extj_namespace.c core/blk_image_sparse.c core/file_range.c core/ticket_lock.c \
    core/strutil.c core/format.c core/fat.c core/fat_write.c core/fat_write_fs.c \
    core/fat_exfat.c core/fat_exfat_fs.c core/ntfs.c core/gpt.c core/iso_stream.c core/rtc.c

# Seed dir for mke2fs -d: a genuinely sparse file, 64 KiB of a known pattern followed by a real
# hole out to 4 MiB. mke2fs -d preserves holes when copying a seed tree in.
mkdir -p "$S/seed"
python3 -c "
import sys
data = bytes(((0x11 + i) & 0xFF) for i in range(65536))
open('$S/seed/hype_sparse.img', 'wb').write(data)
"
truncate -s 4194304 "$S/seed/hype_sparse.img"

IMG="$S/vol.img"
dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
mkfs.ext4 -q -F -b 4096 -O ^orphan_file,^metadata_csum_seed -d "$S/seed" "$IMG"
echo "[ext4] $(mkfs.ext4 -V 2>&1 | head -1)"

check_fsck() { # $1 = label
    e2fsck -fn "$IMG" > "$S/fsck-$1.txt" 2>&1 || {
        echo "FAIL: e2fsck after $1"; cat "$S/fsck-$1.txt"; exit 1;
    }
    echo "[$1] e2fsck -fn clean"
}

alloc_blocks() {
    debugfs -R "stat /hype_sparse.img" "$IMG" 2>/dev/null | grep -oE "Blockcount: [0-9]+" | grep -oE "[0-9]+"
}

check_fsck "seed"
BLOCKS_BEFORE=$(alloc_blocks)
echo "allocated 512B-blocks before any hype write: $BLOCKS_BEFORE (of $((4194304/512)) apparent)"
[ "$BLOCKS_BEFORE" -lt $((4194304/512)) ] || { echo "FAIL: seed file is not actually sparse"; exit 1; }

echo "=== mount-check: confirm hype's own resolver sees the hole ==="
"$S/sparse-harness" "$IMG" /hype_sparse.img mount-check

# LBA 400 = byte 204800, well past the 64 KiB written prefix (LBA 0..127) -- inside the hole.
WRITE_LBA=400
PATTERN=aa

echo "=== write into the hole (this is the growth path) ==="
"$S/sparse-harness" "$IMG" /hype_sparse.img write "$WRITE_LBA" "$PATTERN"
check_fsck "after-write"

BLOCKS_AFTER=$(alloc_blocks)
echo "allocated 512B-blocks after one 512B write: $BLOCKS_AFTER"
[ "$BLOCKS_AFTER" -gt "$BLOCKS_BEFORE" ] || { echo "FAIL: allocation did not grow"; exit 1; }
GREW_BY=$(( (BLOCKS_AFTER - BLOCKS_BEFORE) * 512 ))
# ext4 grows a whole block (4096 bytes) at a time for a 512B write into new territory -- generous
# ceiling (64 KiB) catches "grew by way more than it should have" without being fragile to the
# exact block size.
[ "$GREW_BY" -le 65536 ] || { echo "FAIL: grew by $GREW_BY bytes, way more than one write should cost"; exit 1; }
echo "PASS: allocation grew by $GREW_BY bytes (roughly the write, not the whole 4 MiB)"

echo "=== read-verify: FRESH mount (simulates a VM restart) sees the write ==="
"$S/sparse-harness" "$IMG" /hype_sparse.img read-verify "$WRITE_LBA" "$PATTERN"
check_fsck "after-read-verify"

echo "=== host-side byte check via debugfs (independent of hype's own read path) ==="
debugfs -R "dump /hype_sparse.img $S/dump.bin" "$IMG" >/dev/null 2>&1
python3 - "$S/dump.bin" "$WRITE_LBA" "$PATTERN" <<'PYEOF'
import sys
data = open(sys.argv[1], 'rb').read()
lba = int(sys.argv[2])
pat = int(sys.argv[3], 16)
off = lba * 512
chunk = data[off:off+512]
assert len(chunk) == 512, f"short read at offset {off}: got {len(chunk)} bytes"
assert all(b == pat for b in chunk), f"pattern mismatch at offset {off}"
# the seed prefix (first 64 KiB) must be untouched by the growth write
assert data[0] == 0x11, "seed prefix byte 0 corrupted"
assert data[65535] == ((0x11 + 65535) & 0xFF), "seed prefix tail corrupted"
print("debugfs byte check: OK -- write landed exactly where expected, seed prefix untouched")
PYEOF

echo "ALL PASS: sparse ext4 write-into-hole grows on demand, persists across a fresh mount," \
     "e2fsck -fn clean throughout, allocation grew by roughly the write and not the whole file"
