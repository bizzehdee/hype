#!/bin/sh
# #436: build rig/media.img -- the disk tools/436/run-win.sh boots hype from.
#
# This script lives in tools/ and NOT in build/ for a reason that cost a rebuild:
# `make clean` is `rm -rf build/` (Makefile:195) and build/ is gitignored, so a
# routine clean deletes anything kept there with no copy to recover from. Rig
# scripts belong in tools/<ticket>/ where git holds them; the images they build
# go in rig/, which is on disk (never tmpfs -- that steals the VM's own RAM) and
# is never touched by the build.
#
# Layout, three partitions, because the Windows media cannot be a file on the ESP:
#   p1  ESP, FAT32, 1024 MiB  -- hype.efi, the GUEST firmware pair, hype.cfg, input
#   p2  raw ISO, sized to fit -- the install media, streamed off the bare partition
#   p3  exFAT, the remainder  -- the guest's target disk image
#
# p2 exists because the Win11 media is 5.8 GB and a FAT32 file cannot exceed
# 4 GiB. hype streams the ISO straight off the partition (the GLADDER-10 path,
# `host-stream: vm0 CD001 verified via streaming from partition 2`), so nothing
# ever copies 5.8 GB into a filesystem.
#
# The firmware pair in EFI/hype/ is the GUEST's. The HOST's OVMF is passed to
# QEMU separately by run-win.sh; mixing the two yields a silent 0-byte log.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
RIG="$REPO/rig"
ISO=${ISO:-$REPO/disk-images/Win11_24H2_EnglishInternational_x64.iso}
IMG="$RIG/media.img"

ESP_MB=1024
DATA_MB=${DATA_MB:-24576}

[ -f "$ISO" ] || { echo "ISO not found: $ISO (set ISO=...)"; exit 1; }
[ -f "$REPO/build/hype.efi" ] || { echo "build/hype.efi missing -- run make all"; exit 1; }

mkdir -p "$RIG"
iso_mb=$(( ($(stat -c %s "$ISO") + 1048575) / 1048576 ))
p2_start=$(( 1 + ESP_MB ))
p2_end=$(( p2_start + iso_mb ))
p3_end=$(( p2_end + DATA_MB ))

echo "media.img: ESP ${ESP_MB} MiB | ISO ${iso_mb} MiB (p2) | data ${DATA_MB} MiB (p3)"
rm -f "$IMG" "$RIG/esp.fat"
truncate -s "$(( (p3_end + 4) * 1048576 ))" "$IMG"
parted -s "$IMG" mklabel gpt
parted -s "$IMG" mkpart ESP fat32 1MiB "${p2_start}MiB"
parted -s "$IMG" set 1 esp on
parted -s "$IMG" mkpart media "${p2_start}MiB" "${p2_end}MiB"
parted -s "$IMG" mkpart data "${p2_end}MiB" "${p3_end}MiB"

dd if=/dev/zero of="$RIG/esp.fat" bs=1M count="$ESP_MB" status=none
mkfs.vfat -F 32 -n HYPE436 "$RIG/esp.fat" >/dev/null
mmd -i "$RIG/esp.fat" ::/EFI ::/EFI/BOOT ::/EFI/hype ::/input
mcopy -i "$RIG/esp.fat" "$REPO/build/hype.efi" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$RIG/esp.fat" "$REPO/fw/OVMF_CODE.fd" "$REPO/fw/OVMF_VARS.fd" ::/EFI/hype/
mcopy -i "$RIG/esp.fat" "$HERE/hype.cfg" ::/hype.cfg
[ -f "$HERE/input-vm0.txt" ] && mcopy -i "$RIG/esp.fat" "$HERE/input-vm0.txt" ::/input/vm0.txt
dd if="$RIG/esp.fat" of="$IMG" bs=1M seek=1 conv=notrunc status=none
rm -f "$RIG/esp.fat"

echo "writing the ISO to p2 (${iso_mb} MiB -- the slow part)"
dd if="$ISO" of="$IMG" bs=1M seek="$p2_start" conv=notrunc status=none

# p3 carries the guest's target disk. exFAT because the image is larger than
# FAT32's 4 GiB file cap, and hype reads and writes exFAT (#383).
#
# Built as a standalone file and then written into the partition, the same way
# the ESP above is: mkfs.exfat formats a plain file without root, and udisks
# loop-mounts it as the invoking user. Nothing here needs sudo, which matters
# because the machines this runs on do not all grant it.
dd if=/dev/zero of="$RIG/data.exfat" bs=1M count="$DATA_MB" status=none
mkfs.exfat -n HYPEDATA "$RIG/data.exfat" >/dev/null
LOOP=$(udisksctl loop-setup -f "$RIG/data.exfat" --no-user-interaction |
       sed -n 's/.* as \(\/dev\/loop[0-9]*\).*/\1/p')
MNT=$(udisksctl mount -b "$LOOP" --no-user-interaction |
      sed -n 's/.* at \(.*\)$/\1/p' | sed 's/\.$//')
truncate -s "${TARGET_GB:-20}G" "$MNT/vm0.img"
udisksctl unmount -b "$LOOP" --no-user-interaction >/dev/null
udisksctl loop-delete -b "$LOOP" --no-user-interaction
dd if="$RIG/data.exfat" of="$IMG" bs=1M seek="$p2_end" conv=sparse,notrunc status=none
rm -f "$RIG/data.exfat"

cp -f "$REPO/fw/OVMF_VARS.fd" "$RIG/host-vars.fd"
echo "built:"; ls -l "$IMG"; parted -s "$IMG" print
