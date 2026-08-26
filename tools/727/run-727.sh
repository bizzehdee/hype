#!/bin/bash
# #727: two optical drives on one VM, end to end in QEMU.
#
# This is the rig the bug was found with (#232's validation), reduced to the one
# claim that matters: a VM with install_media AND a cdroms= entry must present
# TWO working discs, and the guest must be able to read the second one.
#
# Asserting from inside the guest is deliberate. hype's own log saying it
# attached a device is a weaker claim than the guest mounting it and reading a
# file back -- and the original bug produced a perfectly clean-looking host log
# for the first disc while the second was never attached at all.
set -eu
cd "$(cd "$(dirname "$0")/../.." && pwd)"
S="${SCRATCH:-/tmp/hype-727}"
ALPINE="disk-images/alpine-standard-3.21.7-x86_64.iso"

[ -f build/hype.efi ] || { echo "build/hype.efi missing -- make all CC=clang LD=ld.lld" >&2; exit 1; }
[ -f "$ALPINE" ] || { echo "missing $ALPINE" >&2; exit 1; }

rm -rf "$S"; mkdir -p "$S/addons"
# The marker is the whole point of the second disc: reading it back proves the
# guest reached THIS medium, not a second node backed by the first one's stream.
echo "HYPE-727-SECOND-DISC-OK" > "$S/addons/HYPE727.TXT"
genisoimage -quiet -J -r -V HYPE727 -o "$S/addons.iso" "$S/addons"

dd if=/dev/zero of="$S/scratch.img" bs=1M count=64 conv=fsync status=none
ESP="$S/esp.img"
dd if=/dev/zero of="$ESP" bs=1M count=1600 conv=fsync status=none
sfdisk --label gpt -q "$ESP" <<SFDISK
2048,,U
SFDISK
mformat -i "$ESP@@1M" -F ::
mmd -i "$ESP@@1M" ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/hype ::/hype/disks ::/input
mcopy -i "$ESP@@1M" build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP@@1M" fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i "$ESP@@1M" "$ALPINE" ::/iso/test.iso
mcopy -i "$ESP@@1M" "$S/addons.iso" ::/iso/addons.iso
mcopy -i "$ESP@@1M" "$S/scratch.img" ::/hype/disks/scratch.img
mcopy -i "$ESP@@1M" tools/727/hype-2cd.cfg ::/hype.cfg
mcopy -i "$ESP@@1M" tools/727/input-vm0.txt ::/input/vm0.txt

cp /usr/share/edk2/ovmf/OVMF_VARS.fd "$S/VARS.fd"
timeout "${SECS:-600}" qemu-system-x86_64 -machine q35 -m 4096 -nodefaults \
  -accel kvm -cpu host -smp 2 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file="$S/VARS.fd" \
  -device ich9-ahci,id=ahci \
  -drive format=raw,file="$ESP",if=none,id=d0 \
  -device ide-hd,drive=d0,bus=ahci.0,serial=HYPEESPDISK,bootindex=0 \
  -serial "file:$S/boot.log" -display none -vga none || true

echo "=== log: $S/boot.log"
grep -aE 'optical drive|cdrom|SCRIPT vm0|SR-|MOUNT1-|HYPE-727|TWOCD-DONE' "$S/boot.log" | tail -25
