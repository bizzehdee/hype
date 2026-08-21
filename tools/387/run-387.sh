#!/bin/bash
# #387 validation: two serialized USB sticks (A = hype's boot/log medium, B = a media-only
# claim) on ONE qemu-xhci controller. PASS = B claimed as MEDIA with its own bulk rings, both
# registered, the VM's ISO resolved+streamed from B BY SERIAL, and the log still landing on A.
set -e
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-$(mktemp -d /mnt/data/dev/hype/disk-images/rig387.XXXXXX)}"
echo "scratch: $S"
killall -9 qemu-system-x86_64 2>/dev/null || true
sleep 1

# stick A: hype's log medium (plain FAT superfloppy, as the log path expects)
dd if=/dev/zero of="$S"/stickA.img bs=1M count=64 conv=fsync status=none
mkfs.vfat -F 32 -n HYPEUSBA "$S"/stickA.img >/dev/null

# stick B: GPT + FAT32 carrying the ISO (the media resolver's expected shape)
ISOB=$(ls disk-images/alpine-hype-dbg.iso)
SZ=$(( $(stat -c%s "$ISOB") / 1048576 + 96 ))
dd if=/dev/zero of="$S"/stickB.img bs=1M count=$SZ conv=fsync status=none
sfdisk --label gpt -q "$S"/stickB.img <<SFDISK
2048,,U
SFDISK
mformat -i "$S"/stickB.img@@1M -F ::
mmd -i "$S"/stickB.img@@1M ::/iso
mcopy -i "$S"/stickB.img@@1M "$ISOB" ::/iso/test.iso

# the boot ESP (AHCI): hype + firmware + config; NO iso here -- it must come from B
dd if=/dev/zero of="$S"/esp.img bs=1M count=512 conv=fsync status=none
sfdisk --label gpt -q "$S"/esp.img <<SFDISK
2048,,U
SFDISK
mformat -i "$S"/esp.img@@1M -F ::
mmd -i "$S"/esp.img@@1M ::/EFI ::/EFI/BOOT ::/EFI/hype
mcopy -i "$S"/esp.img@@1M build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$S"/esp.img@@1M fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i "$S"/esp.img@@1M tools/387/hype.cfg ::/hype.cfg

cp /usr/share/edk2/ovmf/OVMF_VARS.fd "$S"/VARS.fd
timeout "${1:-300}" qemu-system-x86_64 -machine q35 -m 4096 -nodefaults \
  -accel kvm -cpu host -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file="$S"/VARS.fd \
  -device ich9-ahci,id=ahci \
  -drive format=raw,file="$S"/esp.img,if=none,id=d0 \
  -device ide-hd,drive=d0,bus=ahci.0,bootindex=0 \
  -device qemu-xhci,id=xhci \
  -drive format=raw,file="$S"/stickA.img,if=none,id=ua \
  -device usb-storage,bus=xhci.0,drive=ua,serial=HYPE387A \
  -drive format=raw,file="$S"/stickB.img,if=none,id=ub \
  -device usb-storage,bus=xhci.0,drive=ub,serial=HYPE387B \
  -serial "file:$S/serial.txt" -display none -vga none || true

echo "=== claims + registration ==="
LC_ALL=C grep -a "claimed as MEDIA\|media: registered\|second MSC" "$S"/serial.txt | head -6
LC_ALL=C grep -aq "claimed as MEDIA #0 -- serial='HYPE387B'\|claimed as MEDIA #0 -- serial='HYPE387A'" "$S"/serial.txt || { echo "FAIL: no media-only claim"; exit 1; }
echo "=== the VM's media resolved by serial ==="
LC_ALL=C grep -a "media_disk\|host-fat: vm0 resolved\|323" "$S"/serial.txt | head -4
LC_ALL=C grep -aq "host-fat: vm0 resolved \\\\iso\\\\test.iso" "$S"/serial.txt || { echo "FAIL: ISO not resolved"; exit 1; }
echo "=== guest reached its installer boot ==="
LC_ALL=C grep -a "ttyS0| .*login\|BdsDxe" "$S"/serial.txt | head -2
LC_ALL=C grep -aq "localhost login" "$S"/serial.txt || { echo "FAIL: guest never booted from B's ISO"; exit 1; }
echo "=== the log still lands on stick A ==="
mdir -i "$S"/stickA.img ::/ | head -8
mdir -i "$S"/stickA.img ::/ | LC_ALL=C grep -qi "HYPE" || { echo "FAIL: no log files on A"; exit 1; }
echo "ALL PASS: second stick claimed as media, streamed by serial, log intact on the first"
