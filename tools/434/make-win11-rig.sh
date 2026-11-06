#!/bin/sh
# #434: build rig/media.img -- the disk tools/789/run-789.sh boots hype from for the Windows 11
# Secure Boot + TPM 2.0 compat-gate validation.
#
# Same #436-pattern layout (p1 ESP, p2 raw ISO, p3 the target disk -- see that script's own
# comment for why the ISO cannot be a FAT32 file), with one addition: the ESP also carries the
# vendored Secure-Boot-capable guest firmware pair (fw/OVMF_CODE.secboot.fd/OVMF_VARS.secboot.fd,
# #432) alongside the plain pair, since tools/434/hype.cfg's `firmware = uefi-secboot` selects it
# at \EFI\hype\OVMF_CODE.secboot.fd.
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
[ -f "$REPO/fw/OVMF_CODE.secboot.fd" ] && [ -f "$REPO/fw/OVMF_VARS.secboot.fd" ] || {
    echo "fw/OVMF_CODE.secboot.fd / OVMF_VARS.secboot.fd missing -- run"
    echo "  FW_SECBOOT=1 tools/build-fw.sh && tools/enroll-secboot.sh"
    exit 1
}

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
mcopy -i "$RIG/esp.fat" "$REPO/fw/OVMF_CODE.secboot.fd" "$REPO/fw/OVMF_VARS.secboot.fd" ::/EFI/hype/
mcopy -i "$RIG/esp.fat" "$HERE/hype.cfg" ::/hype.cfg
[ -f "$HERE/input-vm0.txt" ] && mcopy -i "$RIG/esp.fat" "$HERE/input-vm0.txt" ::/input/vm0.txt
dd if="$RIG/esp.fat" of="$IMG" bs=1M seek=1 conv=notrunc status=none
rm -f "$RIG/esp.fat"

echo "writing the ISO to p2 (${iso_mb} MiB -- the slow part)"
dd if="$ISO" of="$IMG" bs=1M seek="$p2_start" conv=notrunc status=none

# p3: the guest's target disk, exFAT (same reasoning as #436's own script -- larger than FAT32's
# 4 GiB file cap, and hype reads/writes exFAT, #383).
dd if=/dev/zero of="$RIG/data.exfat" bs=1M count="$DATA_MB" status=none
mkfs.exfat -n HYPEDATA "$RIG/data.exfat" >/dev/null
LOOP=$(udisksctl loop-setup -f "$RIG/data.exfat" --no-user-interaction |
       sed -n 's/.* as \(\/dev\/loop[0-9]*\).*/\1/p')
MNT=$(udisksctl mount -b "$LOOP" --no-user-interaction |
      sed -n 's/.* at \(.*\)$/\1/p' | sed 's/\.$//')
truncate -s "${TARGET_GB:-64}G" "$MNT/vm0.img"
udisksctl unmount -b "$LOOP" --no-user-interaction >/dev/null
udisksctl loop-delete -b "$LOOP" --no-user-interaction
dd if="$RIG/data.exfat" of="$IMG" bs=1M seek="$p2_end" conv=sparse,notrunc status=none
rm -f "$RIG/data.exfat"

cp -f "$REPO/fw/OVMF_VARS.fd" "$RIG/host-vars.fd"
echo "built:"; ls -l "$IMG"; parted -s "$IMG" print
