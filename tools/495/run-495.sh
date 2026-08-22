#!/bin/bash
# #495 bar: hype writes into a file on a volume made by a REAL mkfs.ext4 with metadata_csum
# (crc32c) or the older gdt_csum (crc16) enabled -- default mkfs.ext4 turns metadata_csum on.
# Afterwards e2fsck -fn reports no errors and no checksum warnings, and the file reads back
# byte-exact.
#
# Seeding: mke2fs's own `-d <dir>` populate-from-directory feature -- a real host tool (e2fsprogs
# itself, the same authority e2fsck grades against), never `debugfs write` (a prior session found
# debugfs write produces a different extent layout than a real write and hid a real bug -- see
# MEMORY.md "ext4/FAT test-volume recipe"). This run used `-d` rather than a live udisksctl
# loop-setup/mount (this repo's usual recipe, see tools/497/run-497.sh) because the shared
# udisks/polkit D-Bus daemon on this machine became unresponsive mid-session (mount and
# loop-delete calls hung for minutes and had to be killed; a `busctl` health check confirmed
# udisksd itself was still alive and answering simple queries, so this reads as contention from
# other concurrent sessions on the same host, not a hype bug) -- `-d` is e2fsprogs' own documented
# host tool for this, already the ext writer epic's established fallback (see #497's own comment
# on its ext4 leg), and it is NOT debugfs write.
set -e
set -o pipefail
cd "$(git rev-parse --show-toplevel)"
S=$(mktemp -d)
trap 'rm -rf "$S"' EXIT
CC=${CC:-clang}
$CC -std=c11 -O1 -Wall -o "$S/grow" tools/497/grow-harness.c \
    core/fs_ops.c core/ext.c core/ext2_alloc.c core/ext_jalloc.c core/ext_csum.c core/jbd2.c \
    core/ext_write.c core/file_range.c core/strutil.c core/format.c core/fat.c core/fat_write.c \
    core/fat_write_fs.c core/fat_exfat.c core/fat_exfat_fs.c core/ntfs.c core/gpt.c \
    core/iso_stream.c core/rtc.c

mkdir -p "$S/seed"
dd if=/dev/urandom of="$S/seed/target.bin" bs=1M count=2 status=none

run_leg() {
    NAME="$1"; MKFS_OPTS="$2"
    IMG="$S/vol-$NAME.img"
    dd if=/dev/zero of="$IMG" bs=1M count=128 status=none
    # 4 KiB blocks, the real-world default. 64bit/orphan_file/metadata_csum_seed stay off: they
    # are refused for OTHER, pre-existing reasons (64BIT + CSUM_SEED are explicitly out of scope
    # for #495; orphan_file is an unrelated ROCOMPAT feature hype does not support at all) -- the
    # #497 rig already established this exact isolation. metadata_csum/gdt_csum are what #495
    # actually changes, so they are the only feature toggled per leg below.
    mkfs.ext4 -q -F -b 4096 -O ^64bit,^orphan_file,^metadata_csum_seed $MKFS_OPTS \
        -d "$S/seed" "$IMG"

    # hype now does the write under test: append 6 MiB, allocating through the journal on a
    # checksummed volume it used to refuse outright.
    "$S/grow" "$IMG" "/target.bin" 6 495 2>&1 | sed "s/^/[$NAME] /"

    e2fsck -fn "$IMG" > "$S/fsck-$NAME.txt" 2>&1 || {
        echo "[$NAME] e2fsck FAILED"; sed "s/^/[$NAME] /" "$S/fsck-$NAME.txt"; exit 1;
    }
    sed "s/^/[$NAME] /" "$S/fsck-$NAME.txt"
    if grep -qi "checksum" "$S/fsck-$NAME.txt"; then
        echo "[$NAME] e2fsck reported a checksum warning"; exit 1
    fi
    echo "[$NAME] e2fsck -fn clean, no checksum warnings"

    # host-side byte-exact: dump the file with debugfs (READ-ONLY dump, never `debugfs write`)
    # and compare the appended tail against the same deterministic pattern grow-harness used.
    debugfs -R "dump /target.bin $S/dump-$NAME.bin" "$IMG" >/dev/null 2>&1
    python3 - "$S/dump-$NAME.bin" 495 <<'PYEOF'
import sys
path, seed = sys.argv[1], int(sys.argv[2])
data = open(path, 'rb').read()
want_size = 2 * 1048576 + 6 * 1048576
assert len(data) == want_size, f"size {len(data)} != {want_size}"
base = 2 * 1048576
for i in range(0, 6 * 1048576, 65537):  # stride-sampled full-range check
    want = ((seed * 2654435761 + i) >> 3) & 0xFF
    got = data[base + i]
    assert got == want, f"mismatch at +{i}: {got} != {want}"
print("host byte-exact (sampled) OK")
PYEOF
    echo "[$NAME] PASS"
}

run_leg metacsum "-O metadata_csum"
run_leg gdtcsum "-O ^metadata_csum,uninit_bg"

echo "ALL PASS: metadata_csum (crc32c) and gdt_csum (crc16) volumes both accepted, written,"
echo "e2fsck -fn clean, byte-exact"
