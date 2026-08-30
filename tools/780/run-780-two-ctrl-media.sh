#!/bin/bash
# #780 regression: a USB disk on a SECOND xHCI controller must not rename the first.
#
# Boot 33 on the 5950X booted the guest to "no bootable option or device". hype had registered
# its boot medium as media device 1 under serial 'DB9876543214E', then probed a mass-storage
# device on the OTHER controller -- and because the registration stored a POINTER to the single
# shared g_hostusb_serial buffer that every identity capture rewrites, device 1 silently took
# the second disk's name. media_disk then matched nothing and hype refused to stream media from
# a drive it could not name, which is the correct refusal applied to a corrupted fact.
#
# tools/387 covers two sticks on ONE controller and passed throughout, because the extra-MSC
# sweep copies its serial. Only a SECOND CONTROLLER runs the primary path twice, which is what
# this rig adds.
#
# PASS = the boot medium keeps its own serial in the registration table, media_disk resolves to
# it, and the guest boots from the ISO on it.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-$(mktemp -d /mnt/data/dev/hype/disk-images/rig780.XXXXXX)}"
echo "scratch: $S"
killall -9 "$(basename "$QEMU")" 2>/dev/null || true
sleep 1

# Stick A, on controller 0: hype's log medium AND the media the VM is told to use by serial.
# It is registered FIRST, so it is the one the bug renamed.
ISOB=$(ls disk-images/alpine-hype-dbg.iso)
SZ=$(( $(stat -c%s "$ISOB") / 1048576 + 96 ))
dd if=/dev/zero of="$S"/stickA.img bs=1M count=$SZ conv=fsync status=none
sfdisk --label gpt -q "$S"/stickA.img <<SFDISK
2048,,U
SFDISK
mformat -i "$S"/stickA.img@@1M -F ::
mmd -i "$S"/stickA.img@@1M ::/iso
mcopy -i "$S"/stickA.img@@1M "$ISOB" ::/iso/test.iso

# Stick B, on controller 1: a bystander. Nothing points at it. Its only job is to be probed by
# the primary path on a second controller, which is what corrupted A's identity.
dd if=/dev/zero of="$S"/stickB.img bs=1M count=64 conv=fsync status=none
mkfs.vfat -F 32 -n BYSTANDER "$S"/stickB.img >/dev/null

# The boot ESP on AHCI: hype + config, no ISO -- the ISO must come from stick A by serial.
dd if=/dev/zero of="$S"/esp.img bs=1M count=512 conv=fsync status=none
sfdisk --label gpt -q "$S"/esp.img <<SFDISK
2048,,U
SFDISK
mformat -i "$S"/esp.img@@1M -F ::
mmd -i "$S"/esp.img@@1M ::/EFI ::/EFI/BOOT ::/EFI/hype
make all >/dev/null 2>&1
mcopy -i "$S"/esp.img@@1M build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$S"/esp.img@@1M fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i "$S"/esp.img@@1M tools/780/hype.cfg ::/hype.cfg

cp /usr/share/edk2/ovmf/OVMF_VARS.fd "$S"/VARS.fd
timeout "${1:-300}" "$QEMU" -machine q35 -m 4096 -nodefaults \
  -accel kvm -cpu host -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file="$S"/VARS.fd \
  -device ich9-ahci,id=ahci \
  -drive format=raw,file="$S"/esp.img,if=none,id=d0 \
  -device ide-hd,drive=d0,bus=ahci.0,bootindex=0 \
  -device qemu-xhci,id=xhci0 \
  -drive format=raw,file="$S"/stickA.img,if=none,id=ua \
  -device usb-storage,bus=xhci0.0,drive=ua,serial=HYPE780A \
  -device qemu-xhci,id=xhci1 \
  -drive format=raw,file="$S"/stickB.img,if=none,id=ub \
  -device usb-storage,bus=xhci1.0,drive=ub,serial=HYPE780B \
  -serial "file:$S/serial.txt" -display none -vga none || true

echo "=== two controllers were seen ==="
LC_ALL=C grep -a "host-xhci: controller\[" "$S"/serial.txt | head -4
LC_ALL=C grep -aqc "host-xhci: controller\[2\]" "$S"/serial.txt || {
  echo "INVALID: only one controller enumerated -- this rig proves nothing"; exit 2; }

echo "=== the registration table ==="
LC_ALL=C grep -a "media: registered host device" "$S"/serial.txt | head -6

echo "=== A must still be registered under its OWN serial ==="
LC_ALL=C grep -aq "media: registered host device .* = usb serial='HYPE780A'" "$S"/serial.txt || {
  echo "FAIL: stick A was never registered under HYPE780A"; exit 1; }
# The defect: A registered correctly, then B's probe overwrote the stored pointer, so the
# named drive read as absent. That refusal is the visible symptom.
LC_ALL=C grep -a "is not present -- refusing" "$S"/serial.txt | head -3
LC_ALL=C grep -aq "media_disk = 'HYPE780A' is not present" "$S"/serial.txt && {
  echo "FAIL: media_disk HYPE780A reported absent -- the identity was overwritten (#780)"; exit 1; }

echo "=== the ISO resolved from A, by serial ==="
LC_ALL=C grep -a "host-fat: vm0 resolved\|media_disk" "$S"/serial.txt | head -4
LC_ALL=C grep -aq "host-fat: vm0 resolved \\\\iso\\\\test.iso" "$S"/serial.txt || {
  echo "FAIL: ISO not resolved from stick A"; exit 1; }

echo "=== the guest booted ==="
LC_ALL=C grep -aq "localhost login" "$S"/serial.txt || {
  echo "FAIL: guest never reached a login prompt"; exit 1; }

echo "ALL PASS: a disk on the second controller did not rename the first; media_disk resolved [#780]"
