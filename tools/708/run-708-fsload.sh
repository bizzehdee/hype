#!/bin/bash
#
# #708 follow-up: reproduce the i5 host freeze with the guest images on the SAME host
# filesystems the hw-val drive uses.
#
# Three i5 boots froze the whole host within a second of a guest issuing
# `dd ... of=/dev/vda bs=1M count=64 oflag=direct` to its image:
#   bootI5 2026-09-12c and 2026-09-13a  run2n, image on the drive's NTFS partition
#   bootI5 2026-09-13b                  run2e, image on the drive's ext4 partition
#                                       (run2n's own 64 MiB write had already PASSED)
# So it is not one filesystem, not the first write, and not the view switch.
#
# tools/708/run-708-usbload.sh put every file on ONE FAT32 partition and PASSED -- it never
# takes the ext/NTFS in-place writer at all. This rig mirrors the drive instead:
#   p1 FAT32  HYPEBOOT   hype, firmware, hype.cfg, input scripts, and hype's own log
#   p2 exFAT  HYPEDATA   \hype\disks\run2c-scratch.img
#   p3 ext4   HYPEEXT4   \iso\test.iso      + \hype\disks\ext4-scratch.img   (#688)
#   p4 NTFS   HYPENTFS   \iso\ntfs-test.iso + \hype\disks\ntfs-scratch.img   (#689)
# on one QEMU usb-storage device whose serial is the drive's own, so the staged hype.cfg
# is used verbatim. hype's serial is a file, so a freeze cannot lose the tail.
#
# FREEZE = FBSPEED stops for 30 s after the first READBACK-MATCH. PASS = all three scripts PASS.
#
#   EFI=<path> tools/708/run-708-fsload.sh     default EFI: rig/stage-current/hype-default.efi
set -u
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"

EFI="${EFI:-rig/stage-current/hype-default.efi}"
S="${SCRATCH:-rig/708-fsload}"
ISO=disk-images/hwval-data-2026-09-09/iso/test.iso
HERE=tools/hw-val-2026-08-25
CFG="${CFG:-$HERE/hype2h.cfg}"
[ -f "$EFI" ] || { echo "no $EFI"; exit 2; }
[ -f "$ISO" ] || { echo "no ISO at $ISO"; exit 2; }

# udisks loop-mounts, so no root: the ext4 image is made with root_owner=1000:1000 and the
# exFAT/NTFS ones mount as the caller anyway. debugfs write is NOT used -- it lays a big file
# down in ~100 extents and hype's resolver caps at 64 (see the ext4/FAT test-volume recipe).
MOUNTED=()
fs_mount() { # $1 = image -> echoes the mount point
    local l dev m mp
    l=$(udisksctl loop-setup -f "$1" --no-user-interaction) || return 1
    dev=$(echo "$l" | grep -oE '/dev/loop[0-9]+')
    MOUNTED+=("$dev")
    # udisks may already have auto-mounted the new loop device; asking again is an error.
    for _ in 1 2 3 4 5; do
        mp=$(lsblk -no MOUNTPOINT "$dev" | head -1)
        [ -n "$mp" ] && break
        sleep 1
    done
    if [ -z "$mp" ]; then
        m=$(udisksctl mount -b "$dev" --no-user-interaction) || return 1
        mp=$(echo "$m" | sed -E 's/^Mounted .* at //; s/\.$//')
    fi
    [ -n "$mp" ] || return 1
    echo "$mp"
}
fs_umount_all() {
    local dev
    for dev in "${MOUNTED[@]:-}"; do
        [ -n "$dev" ] || continue
        udisksctl unmount -b "$dev" --no-user-interaction >/dev/null 2>&1
        udisksctl loop-delete -b "$dev" --no-user-interaction >/dev/null 2>&1
    done
    MOUNTED=()
}
trap fs_umount_all EXIT

rm -rf "$S"; mkdir -p "$S"

# ---- p1 FAT32: hype, firmware, config, input scripts (mtools, no mount needed) -------------
dd if=/dev/zero of="$S"/p1.img bs=1M count=640 status=none
mformat -i "$S"/p1.img -F -v HYPEBOOT ::
mmd -i "$S"/p1.img ::/EFI ::/EFI/BOOT ::/EFI/hype ::/input
mcopy -i "$S"/p1.img "$EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$S"/p1.img fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i "$S"/p1.img "$CFG" ::/hype.cfg
for v in 0 1 2 3; do
    [ -f "$HERE/input-2h/vm$v.txt" ] && mcopy -i "$S"/p1.img "$HERE/input-2h/vm$v.txt" "::/input/vm$v.txt"
done

# ---- a fully allocated 1 GiB scratch image, copied into each data volume ------------------
dd if=/dev/zero of="$S"/zero1g.img bs=1M count=1024 status=none

# ---- p2 exFAT: run2c's image --------------------------------------------------------------
dd if=/dev/zero of="$S"/p2.img bs=1M count=1100 status=none
mkfs.exfat -L HYPEDATA "$S"/p2.img >/dev/null
MP=$(fs_mount "$S"/p2.img) || { echo "p2 mount failed"; exit 2; }
mkdir -p "$MP/hype/disks"; cp "$S"/zero1g.img "$MP/hype/disks/run2c-scratch.img"
sync; fs_umount_all

# ---- p3 ext4: the #688 ISO and image -------------------------------------------------------
dd if=/dev/zero of="$S"/p3.img bs=1M count=1700 status=none
# -O ^orphan_file,^metadata_csum_seed: this repo's standing mkfs.ext4 options (#495-#498, #507,
# #508). A default Fedora volume carries both, and hype's ext driver does not read it -- the
# first cut of this rig lost run2c AND run2e that way, with only "NOT FOUND on any of GPT
# partitions 1-4" to say so.
mkfs.ext4 -q -F -b 4096 -O ^orphan_file,^metadata_csum_seed -E root_owner=1000:1000 \
    -L HYPEEXT4 "$S"/p3.img
MP=$(fs_mount "$S"/p3.img) || { echo "p3 mount failed"; exit 2; }
mkdir -p "$MP/iso" "$MP/hype/disks"
cp "$ISO" "$MP/iso/test.iso"; cp "$S"/zero1g.img "$MP/hype/disks/ext4-scratch.img"
sync; fs_umount_all

# ---- p4 NTFS: the #689 ISO and image -------------------------------------------------------
dd if=/dev/zero of="$S"/p4.img bs=1M count=1700 status=none
mkfs.ntfs -q -f -F -L HYPENTFS "$S"/p4.img >/dev/null
MP=$(fs_mount "$S"/p4.img) || { echo "p4 mount failed"; exit 2; }
mkdir -p "$MP/iso" "$MP/hype/disks"
cp "$ISO" "$MP/iso/ntfs-test.iso"; cp "$S"/zero1g.img "$MP/hype/disks/ntfs-scratch.img"
sync; fs_umount_all
sync
rm -f "$S"/zero1g.img

# ---- assemble the whole drive ---------------------------------------------------------------
dd if=/dev/zero of="$S"/usb.img bs=1M count=5150 conv=fsync status=none
sfdisk --label gpt -q "$S"/usb.img <<'SFDISK'
start=2048,      size=1310720, type=uefi,  name=HYPEBOOT
start=1312768,   size=2252800, type=linux, name=HYPEDATA
start=3565568,   size=3481600, type=linux, name=HYPEEXT4
start=7047168,   size=3481600, type=linux, name=HYPENTFS
SFDISK
dd if="$S"/p1.img of="$S"/usb.img bs=512 seek=2048    conv=notrunc status=none
dd if="$S"/p2.img of="$S"/usb.img bs=512 seek=1312768 conv=notrunc status=none
dd if="$S"/p3.img of="$S"/usb.img bs=512 seek=3565568 conv=notrunc status=none
dd if="$S"/p4.img of="$S"/usb.img bs=512 seek=7047168 conv=notrunc status=none
rm -f "$S"/p1.img "$S"/p2.img "$S"/p3.img "$S"/p4.img

# run3d's physical target: present so the config resolves as it does on the drive; run3d is
# not autostarted, so nothing is written to it.
dd if=/dev/zero of="$S"/target.img bs=1M count=256 status=none
cp /usr/share/edk2/ovmf/OVMF_VARS.fd "$S"/VARS.fd

LOG="$S"/serial.log
fbcount() { grep -a -c "FBSPEED: t=" "$LOG" 2>/dev/null || echo 0; }
{
  for _ in $(seq 1 900); do
    sleep 1
    grep -aq "ttyS0| READBACK-MATCH" "$LOG" 2>/dev/null && break
  done
  echo "first readback at $(date +%T)" >&2
  last=$(fbcount); still=0
  for _ in $(seq 1 900); do
    sleep 1
    now=$(fbcount)
    if [ "$now" = "$last" ]; then still=$((still + 1)); else still=0; last=$now; fi
    [ "$(grep -a -c -E 'SCRIPT vm[0-2]: PASS' "$LOG")" -ge 3 ] && break
    if [ "$still" -ge 30 ]; then
      echo "FBSPEED stalled 30 s at $(date +%T)" >&2
      for i in 1 2 3; do printf 'info registers -a\n'; sleep 3; done
      printf 'screendump %s/freeze.ppm\n' "$S"; sleep 3
      break
    fi
  done
  sleep 5
  printf 'quit\n'
} | timeout 2700 "$QEMU" -machine q35 -m 12288 -nodefaults \
  -accel kvm -cpu host -smp 16,sockets=1,cores=8,threads=2 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file="$S"/VARS.fd \
  -device qemu-xhci,id=xhci \
  -drive format=raw,file="$S"/usb.img,if=none,id=stick \
  -device usb-storage,bus=xhci.0,drive=stick,serial=DB9876543214E,bootindex=0 \
  -drive format=raw,file="$S"/target.img,if=none,id=tgt \
  -device usb-storage,bus=xhci.0,drive=tgt,serial=03025220071724203145 \
  -serial "file:$LOG" -monitor stdio -display none -vga std >"$S"/mon.log 2>"$S"/qemu.err || true

echo "=== build / vendor ==="
grep -a -m2 -E "^hype: build|vmm: (SVM|VMX) detected" "$LOG"
echo "PANIC=$(grep -a -c PANIC "$LOG") APSTACK_OVERFLOW=$(grep -a -c 'APSTACK OVERFLOW' "$LOG")"
echo "=== volumes, disks and log sink ==="
grep -a -E "m5-8: FILE-backed|host-fat: vm[0-9] resolved|usb-log: \\\\HYPE.LOG|XHCIOWN" "$LOG" | cut -c1-160 | head -10
echo "=== stack high-water ==="
grep -a "APSTACK:" "$LOG" | tail -2 | cut -c1-200
echo "=== scripts ==="
grep -a -E "SCRIPT vm[0-9]: (PASS|FAIL)" "$LOG" | cut -c1-120
grep -a -E "vm[0-9] ttyS0\| (READBACK-MATCH|BULK-[0-9]+|EXT4-WRITE-DONE|NTFS-WRITE-DONE|BOOT-OK-)" "$LOG" | cut -c1-100
grep -a -o -E "FBSPEED: t=[0-9]+ms" "$LOG" | tail -1

if ! grep -aq "m5-8: FILE-backed guest disk .*on ext" "$LOG" || \
   ! grep -aq "m5-8: FILE-backed guest disk .*on NTFS" "$LOG"; then
  echo "INVALID: the images did not resolve on ext AND NTFS -- the i5's path was never taken"; exit 2
fi
if ! grep -aq "ttyS0| READBACK-MATCH" "$LOG"; then
  echo "INVALID: no guest read back its signature -- the i5's trigger was never reached"; exit 2
fi
if [ -f "$S"/freeze.ppm ]; then
  echo "FREEZE: the BSP stopped logging after a guest's READBACK-MATCH [#708]"
  grep -a -o -E "CPU#[0-9]+|RIP=[0-9a-f]{16}" "$S"/mon.log | paste - - | sort | uniq -c
  tail -30 "$LOG" | cut -c1-200
  exit 1
fi
if [ "$(grep -a -c -E 'SCRIPT vm[0-2]: PASS' "$LOG")" -ge 3 ]; then
  echo "PASS: all three scripts finished with the images on exFAT, ext4 and NTFS [#708]"; exit 0
fi
echo "INCONCLUSIVE: no freeze detected but not every script passed"; tail -20 "$LOG" | cut -c1-200; exit 3
