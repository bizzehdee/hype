#!/bin/bash
# #498 bar: on ext2, ext3 and a default mkfs.ext4 volume (metadata_csum on),
# hype creates a file, writes to it, makes a directory, renames the file into
# it, then unlinks the file and removes the directory. After EACH step,
# e2fsck -fn reports no errors on all three volume types, and a host-side
# read-only check (debugfs -R -- never `debugfs write`, per this repo's own
# established rule) sees exactly the expected tree.
#
# No seed content is needed here (unlike #495/#496/#497, which wrote into an
# ALREADY-EXISTING file): #498 creates its own file and directory from an
# empty, real mkfs volume, so there is nothing for udisksctl or `mke2fs -d`
# to seed -- the udisksctl-contention concern those tickets hit does not
# apply to this one.
#
# ext4: `-O ^orphan_file,^metadata_csum_seed` is the same exclusion #495/#496
# already established -- everything else (metadata_csum, 64bit, dir_index,
# flex_bg, ...) stays at this e2fsprogs install's real default.
set -e
set -o pipefail
cd "$(git rev-parse --show-toplevel)"
S=$(mktemp -d)
trap 'rm -rf "$S"' EXIT
CC=${CC:-clang}
$CC -std=c11 -O1 -Wall -o "$S/ns" tools/498/ns-harness.c \
    core/fs_ops.c core/ext.c core/ext2_alloc.c core/ext_jalloc.c core/ext_csum.c core/jbd2.c \
    core/ext_write.c core/ext_dirent.c core/ext_namespace.c core/ext2_namespace.c \
    core/extj_namespace.c core/file_range.c core/strutil.c core/format.c core/fat.c \
    core/fat_write.c core/fat_write_fs.c core/fat_exfat.c core/fat_exfat_fs.c core/ntfs.c \
    core/gpt.c core/iso_stream.c core/rtc.c

STEP_NAME=(- create write mkdir rename unlink rmdir)

run_leg() {
    NAME="$1"; MKFS="$2"; shift 2
    IMG="$S/vol-$NAME.img"
    dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
    "$MKFS" -q -F -b 4096 "$@" "$IMG"
    echo "[$NAME] $("$MKFS" -V 2>&1 | head -1)"

    for step in 1 2 3 4 5 6; do
        "$S/ns" "$IMG" "$step" 2>&1 | sed "s/^/[$NAME] /"

        if ! e2fsck -fn "$IMG" > "$S/fsck-$NAME-$step.txt" 2>&1; then
            echo "[$NAME] step $step (${STEP_NAME[$step]}): e2fsck FAILED"
            sed "s/^/[$NAME] /" "$S/fsck-$NAME-$step.txt"
            exit 1
        fi
        echo "[$NAME] step $step (${STEP_NAME[$step]}): e2fsck -fn clean"

        LS_ROOT=$(debugfs -R "ls -l /" "$IMG" 2>/dev/null)
        case $step in
        1)
            echo "$LS_ROOT" | grep -q "hype_test.txt" || { echo "[$NAME] hype_test.txt missing from /"; exit 1; }
            SIZE=$(debugfs -R "stat /hype_test.txt" "$IMG" 2>/dev/null | grep -oE "Size: [0-9]+" | head -1 | grep -oE "[0-9]+")
            [ "$SIZE" = "0" ] || { echo "[$NAME] hype_test.txt not 0 bytes after create (got $SIZE)"; exit 1; }
            LINKS=$(debugfs -R "stat /hype_test.txt" "$IMG" 2>/dev/null | grep -oE "Links: [0-9]+" | head -1 | grep -oE "[0-9]+")
            [ "$LINKS" = "1" ] || { echo "[$NAME] hype_test.txt links != 1 (got $LINKS)"; exit 1; }
            ;;
        2)
            debugfs -R "dump /hype_test.txt $S/dump-$NAME-2.bin" "$IMG" >/dev/null 2>&1
            python3 - "$S/dump-$NAME-2.bin" <<'PYEOF'
import sys
data = open(sys.argv[1], 'rb').read()
assert len(data) == 4096, f"size {len(data)} != 4096"
for i in range(4096):
    want = ((498 * 2654435761 + i) >> 3) & 0xFF
    assert data[i] == want, f"mismatch at +{i}: {data[i]} != {want}"
print("host byte-exact after write: OK")
PYEOF
            ;;
        3)
            echo "$LS_ROOT" | grep -q "hype_dir" || { echo "[$NAME] hype_dir missing from /"; exit 1; }
            debugfs -R "stat /hype_dir" "$IMG" 2>/dev/null | grep -q "directory" || { echo "[$NAME] hype_dir is not a directory"; exit 1; }
            ;;
        4)
            echo "$LS_ROOT" | grep -q "hype_test.txt" && { echo "[$NAME] hype_test.txt still in / after rename"; exit 1; }
            debugfs -R "ls -l /hype_dir" "$IMG" 2>/dev/null | grep -q "hype_test.txt" || { echo "[$NAME] hype_test.txt missing from /hype_dir after rename"; exit 1; }
            ;;
        5)
            debugfs -R "ls -l /hype_dir" "$IMG" 2>/dev/null | grep -q "hype_test.txt" && { echo "[$NAME] hype_test.txt still in /hype_dir after unlink"; exit 1; }
            ;;
        6)
            echo "$LS_ROOT" | grep -q "hype_dir" && { echo "[$NAME] hype_dir still in / after rmdir"; exit 1; }
            ;;
        esac
        echo "[$NAME] step $step (${STEP_NAME[$step]}): host tree matches expectations"
    done
    echo "[$NAME] PASS"
}

run_leg ext2 mkfs.ext2
run_leg ext3 mkfs.ext3
run_leg ext4 mkfs.ext4 -O ^orphan_file,^metadata_csum_seed

echo "ALL PASS: ext2 + ext3 + ext4 (metadata_csum on) create/write/mkdir/rename/unlink/rmdir," \
     "e2fsck -fn clean after every step, host tree matches at every step"
