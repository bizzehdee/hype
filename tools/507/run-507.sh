#!/bin/bash
# #507: the full bar. Boot 1: QMP types `mkdisk HYPE507TGT \disks\vm.img 1 raw-sparse` at the
# dashboard; hype creates a raw image on the SECOND disk's EXT4 volume (serial HYPE507TGT) whose
# virtual size is 1 GiB but which occupies far less. Host-side: du/debugfs + e2fsck confirm it is
# genuinely sparse. Then #506's own already-validated sparse-harness (tools/506) opens the
# JUST-CREATED file and proves it is actually usable -- a write into the hole grows the
# allocation and persists across a fresh mount -- without re-running the virtio-blk/QEMU-guest
# leg #505 already proved end-to-end for the non-sparse case.
set -e
set -o pipefail
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-$(mktemp -d /mnt/data/dev/hype/disk-images/rig507.XXXXXX)}"
echo "scratch: $S"
killall -9 "$(basename "$QEMU")" 2>/dev/null || true
sleep 1

# The target disk: GPT + one EXT4 volume (the only host filesystem that can represent a hole,
# #507's own scope). Build the ext4 filesystem as a standalone image first (mke2fs needs a plain
# file, not a byte offset into a bigger one), then overlay it at the partition's start -- avoids
# loop-mount/udisksctl entirely, matching this repo's own established pattern (#495/#496/#497).
dd if=/dev/zero of="$S"/target.img bs=1M count=1600 conv=fsync status=none
sfdisk --label gpt -q "$S"/target.img <<SFDISK
2048,,L
SFDISK
dd if=/dev/zero of="$S"/ext.img bs=1M count=1590 conv=fsync status=none
mkfs.ext4 -q -F -b 4096 -O ^orphan_file,^metadata_csum_seed "$S"/ext.img
# mkdisk creates the file at \disks\vm.img -- the parent directory must already exist (no
# namespace op auto-creates a missing parent), same as mmd pre-creating ::/disks on the FAT32
# ESP in every other rig. mtools cannot touch ext4, so debugfs -w does it here.
debugfs -w -R "mkdir /disks" "$S"/ext.img >/dev/null 2>&1
dd if="$S"/ext.img of="$S"/target.img bs=1M seek=1 conv=notrunc status=none

build_esp() { # $1 = cfg, $2 = out-esp [, $3 = input script]
    rm -f "$2"
    dd if=/dev/zero of="$2" bs=1M count=1800 conv=fsync status=none
    sfdisk --label gpt -q "$2" <<SFDISK
2048,,U
SFDISK
    mformat -i "$2@@1M" -F ::
    mmd -i "$2@@1M" ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/input ::/hype ::/hype/disks
    mcopy -i "$2@@1M" build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
    mcopy -i "$2@@1M" fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
    mcopy -i "$2@@1M" disk-images/alpine-hype-dbg.iso ::/iso/test.iso
    mcopy -i "$2@@1M" "$1" ::/hype.cfg
    [ -n "${3:-}" ] && mcopy -i "$2@@1M" "$3" ::/input/vm0.txt
    true
}

run_qemu() { # $1 = esp, $2 = log, $3 = seconds [, $4 = qmp-socket]
    local qmp=()
    [ -n "${4:-}" ] && { rm -f "$4"; qmp=(-qmp "unix:$4,server=on,wait=off"); }
    cp /usr/share/edk2/ovmf/OVMF_VARS.fd "$S"/VARS.fd
    timeout "$3" "$QEMU" -machine q35 -m 4096 -nodefaults \
      -accel kvm -cpu host -smp 4 \
      -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
      -drive if=pflash,format=raw,file="$S"/VARS.fd \
      -device ich9-ahci,id=ahci \
      -drive format=raw,file="$1",if=none,id=d0 \
      -device ide-hd,drive=d0,bus=ahci.0,serial=HYPEESPDISK,bootindex=0 \
      -drive format=raw,file="$S"/target.img,if=none,id=d1 \
      -device ide-hd,drive=d1,bus=ahci.1,serial=HYPE507TGT \
      "${qmp[@]}" \
      -serial "file:$2" -display none -vga none || true
}

echo "=== boot 1: mkdisk raw-sparse on the second disk (serial HYPE507TGT, ext4) ==="
build_esp tools/507/hype-boot1.cfg "$S"/esp1.img
run_qemu "$S"/esp1.img "$S"/boot1.log 1200 "$S"/qmp.sock &
QPID=$!
( sleep 45; python3 tools/qmp-sendkeys.py "$S"/qmp.sock "$(cat tools/507/sendkeys.txt)" > "$S"/keys.log 2>&1 ) &
for i in $(seq 1 230); do
    sleep 5
    LC_ALL=C grep -aq "MKDISK: DONE\|MKDISK: FAILED\|mkdisk: FAILED\|raw-sparse refused" \
        "$S"/boot1.log 2>/dev/null && break
done
killall "$(basename "$QEMU")" 2>/dev/null || true
wait $QPID 2>/dev/null || true
LC_ALL=C grep -a "TERMCMD.*mkdisk\|MKDISK" "$S"/boot1.log | head -6
LC_ALL=C grep -aq "MKDISK: DONE" "$S"/boot1.log || { echo "FAIL: mkdisk did not finish"; exit 1; }

echo "=== host-side: extract the ext4 partition and check allocation ==="
dd if="$S"/target.img of="$S"/ext-out.img bs=1M skip=1 count=1590 status=none
e2fsck -fn "$S"/ext-out.img > "$S"/fsck.txt 2>&1 || { echo "FAIL: e2fsck"; cat "$S"/fsck.txt; exit 1; }
echo "e2fsck -fn clean"
debugfs -R "stat /disks/vm.img" "$S"/ext-out.img 2>/dev/null | tee "$S"/stat.txt | grep -E "Size|Blockcount"
SIZE=$(grep -oE "Size: [0-9]+" "$S"/stat.txt | head -1 | grep -oE "[0-9]+")
BLOCKS=$(grep -oE "Blockcount: [0-9]+" "$S"/stat.txt | head -1 | grep -oE "[0-9]+")
[ "$SIZE" -eq 1073741824 ] || { echo "FAIL: virtual size not 1 GiB (got $SIZE)"; exit 1; }
ALLOC_BYTES=$((BLOCKS * 512))
echo "virtual=$SIZE allocated=$ALLOC_BYTES bytes"
[ "$ALLOC_BYTES" -lt 1048576 ] || { echo "FAIL: allocated $ALLOC_BYTES bytes -- not sparse"; exit 1; }
echo "PASS: 1 GiB virtual, ${ALLOC_BYTES} bytes allocated (under 1 MiB)"

echo "=== confirm #506's own machinery can actually use the just-created file ==="
CC=${CC:-clang}
clang -std=c11 -O1 -Wall -o "$S/sparse-harness" tools/506/sparse-harness.c \
    core/fs_ops.c core/ext.c core/ext2_alloc.c core/ext_jalloc.c core/ext_csum.c core/jbd2.c \
    core/ext_write.c core/ext_dirent.c core/ext_namespace.c core/ext2_namespace.c \
    core/extj_namespace.c core/blk_image_sparse.c core/file_range.c core/ticket_lock.c \
    core/strutil.c core/format.c core/fat.c core/fat_write.c core/fat_write_fs.c \
    core/fat_exfat.c core/fat_exfat_fs.c core/ntfs.c core/gpt.c core/iso_stream.c core/rtc.c
"$S/sparse-harness" "$S/ext-out.img" /disks/vm.img mount-check
"$S/sparse-harness" "$S/ext-out.img" /disks/vm.img write 400 bb
e2fsck -fn "$S/ext-out.img" > "$S"/fsck2.txt 2>&1 || { echo "FAIL: e2fsck after growth write"; cat "$S"/fsck2.txt; exit 1; }
"$S/sparse-harness" "$S/ext-out.img" /disks/vm.img read-verify 400 bb

echo "ALL PASS: mkdisk raw-sparse creates a genuinely sparse ext4 image (dashboard-driven, real" \
     "QEMU boot), under 1 MiB allocated for a 1 GiB virtual disk, e2fsck -fn clean, and #506's" \
     "growth path opens and grows it correctly"
