#!/bin/sh
# #343/#311: build disk-images/qbsd.img -- the ESP image tools/311/run-bsd.sh boots.
#
# Separated from run-bsd.sh deliberately: that script refreshes ONLY BOOTX64.EFI per run and
# leaves the 1.3 GB ISO in place, because re-copying it per run dominates the cycle. This builds
# the image once. Re-run it only when the ISO or the firmware pair changes.
#
# Two constraints, both learned the hard way and both recorded in run-bsd.sh already:
#  - a real partitioned FAT image, never vvfat: `-drive file=fat:rw:<dir>` SIGSEGVs QEMU inside its
#    own AHCI emulation once hype writes to the volume (#288), which reads as hype crashing on
#    entry and is not.
#  - the GUEST firmware pair goes in EFI/hype/. hype loads \EFI\hype\OVMF_CODE.fd for the VM; the
#    HOST's OVMF is passed to QEMU separately by run-bsd.sh, and mixing them yields a 0-byte log.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT="$REPO/disk-images"
ISO=${ISO:-$HOME/Downloads/FreeBSD-15.1-RELEASE-amd64-disc1.iso}

[ -f "$ISO" ] || { echo "ISO not found: $ISO (set ISO=...)"; exit 1; }
[ -f "$REPO/build/hype.efi" ] || { echo "build/hype.efi missing -- run make all"; exit 1; }

mkdir -p "$OUT"
cd "$OUT"

iso_mb=$(( ($(stat -c %s "$ISO") + 1048575) / 1048576 ))
esp_mb=$(( iso_mb + 96 ))            # ISO + firmware pair + hype.efi + slack
img_mb=$(( esp_mb + 4 ))

echo "building qbsd.img: ESP ${esp_mb} MiB (ISO ${iso_mb} MiB)"
rm -f qbsd.img qbsd.fat
dd if=/dev/zero of=qbsd.img bs=1M count="$img_mb" status=none
parted -s qbsd.img mklabel gpt
parted -s qbsd.img mkpart ESP fat32 1MiB "$((esp_mb + 1))MiB"
parted -s qbsd.img set 1 esp on

dd if=/dev/zero of=qbsd.fat bs=1M count="$esp_mb" status=none
mkfs.vfat -F 32 -n HYPEBSD qbsd.fat >/dev/null
mmd -i qbsd.fat ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/input
mcopy -i qbsd.fat "$REPO/build/hype.efi" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i qbsd.fat "$REPO/fw/OVMF_CODE.fd" "$REPO/fw/OVMF_VARS.fd" ::/EFI/hype/
# #343: without this the run parks forever on bsdinstall's "Console type [vt100]:" prompt and never
# reaches the window where the fault occurs.
mcopy -i qbsd.fat "$REPO/tools/input-scripts/bsd343-vm0.txt" ::/input/vm0.txt
echo "copying the ISO (1.3 GB -- this is the slow part)"
mcopy -i qbsd.fat "$ISO" ::/iso/test.iso
dd if=qbsd.fat of=qbsd.img bs=512 seek=2048 conv=notrunc status=none
rm -f qbsd.fat

echo "qbsd.img built:"
mdir -i qbsd.img@@1M ::/iso 2>/dev/null || true
ls -l qbsd.img
