#!/bin/bash
# #513 repro: the real-hardware shape in QEMU -- NO hype.cfg (built-in default
# 2 VMs, STARTABLE=2 build), \iso\test.iso + \iso\vm1.iso, USB log stick,
# 8 whole host cores so both VMs get their dedicated cores like the laptop.
set -e
cd "$(dirname "$0")/.."
S=rig/i513
mkdir -p $S
B=build
SECS="${1:-120}"

rm -f $S/usb.img $S/esp.img
dd if=/dev/zero of=$S/usb.img bs=1M count=64 status=none
dd if=/dev/zero of=$S/nvme.img bs=1M count=64 status=none
mkfs.vfat -F 32 -n HYPEUSB $S/usb.img >/dev/null

dd if=/dev/zero of=$S/esp.img bs=1M count=512 status=none
mkfs.vfat -F 32 -n HYPEESP $S/esp.img >/dev/null
mmd -i $S/esp.img ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso
mcopy -i $S/esp.img $B/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i $S/esp.img fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i $S/esp.img disk-images/alpine-virt-console.iso ::/iso/test.iso
mcopy -i $S/esp.img disk-images/alpine-virt-console.iso ::/iso/vm1.iso
# NO hype.cfg -- that is the point.

cp /usr/share/edk2/ovmf/OVMF_VARS.fd $S/VARS.fd
timeout "$SECS" qemu-system-x86_64 \
  -machine q35 -m 8192 -nodefaults \
  -accel kvm -cpu host -smp 8,sockets=1,cores=4,threads=2 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=$S/VARS.fd \
  -drive format=raw,file=$S/esp.img,if=none,id=esp \
  -device ide-hd,drive=esp,bus=ide.0,bootindex=0 \
  -device qemu-xhci,id=xhci \
  -drive format=raw,file=$S/usb.img,if=none,id=stick \
  -device usb-storage,bus=xhci.0,drive=stick \
  -drive format=raw,file=$S/nvme.img,if=none,id=nv0 \
  -device nvme,drive=nv0,serial=HYPE513NVME \
  -serial "file:$S/serial.log" -display none -vga std 2>$S/qemu.err || true
echo "=== serial tail:"; LC_ALL=C tail -3 $S/serial.log | cut -c1-140
