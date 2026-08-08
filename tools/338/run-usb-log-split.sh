#!/bin/bash
# #338 validation: boot hype with an emulated USB stick, then read back the
# split log files the run leaves on it.
set -e
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-$(mktemp -d)}"
B=build

rm -f $S/usb.img $S/esp.img
dd if=/dev/zero of=$S/usb.img bs=1M count=64 status=none
mkfs.vfat -F 32 -n HYPEUSB $S/usb.img >/dev/null

dd if=/dev/zero of=$S/esp.img bs=1M count=512 status=none
mkfs.vfat -F 32 -n HYPEESP $S/esp.img >/dev/null
mmd -i $S/esp.img ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso
mcopy -i $S/esp.img $B/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i $S/esp.img fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i $S/esp.img disk-images/alpine-hype-dbg.iso ::/iso/test.iso 2>/dev/null || \
  mcopy -i $S/esp.img "$(ls disk-images/*.iso | head -1)" ::/iso/test.iso
mcopy -i $S/esp.img tools/qemu-cd-hype.cfg ::/hype.cfg 2>/dev/null || true

for ATTEMPT in 1 2 3; do
cp /usr/share/OVMF/OVMF_VARS.fd $S/VARS.fd
timeout "${1:-150}" qemu-system-x86_64 \
  -machine q35 -m 2048 -nodefaults \
  -accel kvm -accel tcg -cpu host -smp 2 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=$S/VARS.fd \
  -drive format=raw,file=$S/esp.img,if=none,id=esp \
  -device ide-hd,drive=esp,bus=ide.0,bootindex=0 \
  -device qemu-xhci,id=xhci \
  -drive format=raw,file=$S/usb.img,if=none,id=stick \
  -device usb-storage,bus=xhci.0,drive=stick \
  -serial stdio -display none -vga none > $S/serial-$ATTEMPT.txt 2>&1 || true

if grep -aq "usb-log:" $S/serial-$ATTEMPT.txt; then
    echo "attempt $ATTEMPT: hype ran"; cp $S/serial-$ATTEMPT.txt $S/serial.txt; break
  fi
  echo "attempt $ATTEMPT: hype never ran (BdsDxe flake) -- retrying"
done

echo "=== files left on the USB stick ==="
mdir -i $S/usb.img :: || true
