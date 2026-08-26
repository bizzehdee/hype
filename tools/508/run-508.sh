#!/bin/bash
# #508: the full bar. Boot 1: QMP types `mkdisk HYPE508TGT \disks\vm.qcow2 1 qcow2-sparse` at
# the dashboard; hype creates a qcow2 on the SECOND disk's EXT4 volume whose header/refcount/L1
# metadata is real but whose data clusters are an unallocated hole. Host-side: qemu-img check +
# info (the ticket's own bar, verbatim) plus du/debugfs confirm it is genuinely sparse. Then
# tools/508's own harness -- hype_blk_image_sparse_t + hype_qcow2_t layered exactly as
# boot/main.c wires them -- opens the JUST-CREATED file, writes into a data cluster (the growth
# path, one layer under qcow2), and confirms it persists across a fresh mount.
set -e
set -o pipefail
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-$(mktemp -d /mnt/data/dev/hype/disk-images/rig508.XXXXXX)}"
echo "scratch: $S"
killall -9 "$(basename "$QEMU")" 2>/dev/null || true
sleep 1

dd if=/dev/zero of="$S"/target.img bs=1M count=1600 conv=fsync status=none
sfdisk --label gpt -q "$S"/target.img <<SFDISK
2048,,L
SFDISK
dd if=/dev/zero of="$S"/ext.img bs=1M count=1590 conv=fsync status=none
mkfs.ext4 -q -F -b 4096 -O ^orphan_file,^metadata_csum_seed "$S"/ext.img
# mkdisk creates the file at \disks\vm.qcow2 -- the parent directory must already exist, same
# as #507's rig (see its comment for why).
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
      -device ide-hd,drive=d1,bus=ahci.1,serial=HYPE508TGT \
      "${qmp[@]}" \
      -serial "file:$2" -display none -vga none || true
}

echo "=== boot 1: mkdisk qcow2-sparse on the second disk (serial HYPE508TGT, ext4) ==="
build_esp tools/508/hype-boot1.cfg "$S"/esp1.img
run_qemu "$S"/esp1.img "$S"/boot1.log 1200 "$S"/qmp.sock &
QPID=$!
( sleep 45; python3 tools/qmp-sendkeys.py "$S"/qmp.sock "$(cat tools/508/sendkeys.txt)" > "$S"/keys.log 2>&1 ) &
for i in $(seq 1 230); do
    sleep 5
    LC_ALL=C grep -aq "MKDISK: DONE\|MKDISK: FAILED\|mkdisk: FAILED\|qcow2-sparse refused" \
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
debugfs -R "stat /disks/vm.qcow2" "$S"/ext-out.img 2>/dev/null | tee "$S"/stat.txt | grep -E "Size|Blockcount"
FILESIZE=$(grep -oE "Size: [0-9]+" "$S"/stat.txt | head -1 | grep -oE "[0-9]+")
BLOCKS=$(grep -oE "Blockcount: [0-9]+" "$S"/stat.txt | head -1 | grep -oE "[0-9]+")
ALLOC_BYTES=$((BLOCKS * 512))
echo "on-disk file size=$FILESIZE allocated=$ALLOC_BYTES bytes"
# generous ceiling: header + refcount table/blocks + L1 + L2 for a 1 GiB image at 64 KiB
# clusters is a handful of clusters, nowhere near the 1 GiB virtual size.
[ "$ALLOC_BYTES" -lt 4194304 ] || { echo "FAIL: allocated $ALLOC_BYTES bytes -- not sparse"; exit 1; }
echo "PASS: file size $FILESIZE bytes, ${ALLOC_BYTES} bytes actually allocated"

echo "=== host-side: qemu-img check + info (the ticket's own bar, verbatim) ==="
# ext4, not FAT -- mtools cannot read it; debugfs is the read-only extraction tool this repo's
# own scripts already standardize on for ext images.
debugfs -R "dump /disks/vm.qcow2 $S/vm.qcow2" "$S"/ext-out.img >/dev/null 2>&1
qemu-img check "$S"/vm.qcow2 2>&1 | tail -3
qemu-img check "$S"/vm.qcow2 >/dev/null 2>&1 || { echo "FAIL: qemu-img check"; exit 1; }
qemu-img info "$S"/vm.qcow2 | grep -E "virtual size|file format|disk size"
qemu-img info "$S"/vm.qcow2 | grep -q "virtual size: 1 GiB" || { echo "FAIL: not 1 GiB virtual"; exit 1; }
echo "PASS: qemu-img check clean, virtual size 1 GiB"

echo "=== confirm the sparse-qcow2 layering can actually grow it (a guest-write equivalent) ==="
CC=${CC:-clang}
clang -std=c11 -O1 -Wall -o "$S/qcow2-sparse-harness" tools/508/qcow2-sparse-harness.c \
    core/fs_ops.c core/ext.c core/ext2_alloc.c core/ext_jalloc.c core/ext_csum.c core/jbd2.c \
    core/ext_write.c core/ext_dirent.c core/ext_namespace.c core/ext2_namespace.c \
    core/extj_namespace.c core/blk_image_sparse.c core/blk_qcow2.c core/blk_backend.c \
    core/file_range.c core/ticket_lock.c core/strutil.c core/format.c core/fat.c \
    core/fat_write.c core/fat_write_fs.c core/fat_exfat.c core/fat_exfat_fs.c core/ntfs.c \
    core/gpt.c core/iso_stream.c core/rtc.c
"$S/qcow2-sparse-harness" "$S/ext-out.img" /disks/vm.qcow2 mount-check
"$S/qcow2-sparse-harness" "$S/ext-out.img" /disks/vm.qcow2 write 4096 cc
e2fsck -fn "$S/ext-out.img" > "$S"/fsck2.txt 2>&1 || { echo "FAIL: e2fsck after growth write"; cat "$S"/fsck2.txt; exit 1; }
"$S/qcow2-sparse-harness" "$S/ext-out.img" /disks/vm.qcow2 read-verify 4096 cc

echo "=== host-side: qemu-img check STILL clean after the guest write ==="
debugfs -R "dump /disks/vm.qcow2 $S/vm2.qcow2" "$S"/ext-out.img >/dev/null 2>&1
qemu-img check "$S/vm2.qcow2" >/dev/null 2>&1 || { echo "FAIL: qemu-img check after write"; exit 1; }
echo "PASS: qemu-img check still clean after a guest write grew a data cluster"

echo "ALL PASS: mkdisk qcow2-sparse creates a genuinely sparse qcow2 (dashboard-driven, real" \
     "QEMU boot), qemu-img check/info both clean, e2fsck -fn clean throughout, and a write" \
     "through the sparse-image layering grows and persists correctly"
