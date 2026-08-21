#!/bin/bash
# #589 leg: a guest disk file lives on the SECOND AHCI port's FAT32 volume. PASS = the media
# registry (now one device per inventoried port) resolves the file there and the guest reads it.
# Before #589 only the boot disk (ahci.0) was a media device, so the file was NOT FOUND.
set -e
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-$(mktemp -d /mnt/data/dev/hype/disk-images/rig589.XXXXXX)}"
echo "scratch: $S"
killall -9 qemu-system-x86_64 2>/dev/null || true
sleep 1

# The SECOND disk: GPT + one FAT32 volume holding \disks\vda.img (the guest's disk).
dd if=/dev/zero of="$S"/target.img bs=1M count=200 conv=fsync status=none
sfdisk --label gpt -q "$S"/target.img <<SFDISK
2048,,U
SFDISK
mformat -i "$S"/target.img@@1M -F ::
mmd -i "$S"/target.img@@1M ::/disks
mcopy -i "$S"/target.img@@1M disk-images/589/vda.img ::/disks/vda.img

# The ESP (boot disk, ahci.0): hype + kernel + config + input.
ESP="$S"/esp.img
dd if=/dev/zero of="$ESP" bs=1M count=200 conv=fsync status=none
sfdisk --label gpt -q "$ESP" <<SFDISK
2048,,U
SFDISK
mformat -i "$ESP@@1M" -F ::
mmd -i "$ESP@@1M" ::/EFI ::/EFI/BOOT ::/EFI/hype ::/EFI/hype/micro ::/input
mcopy -i "$ESP@@1M" build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP@@1M" fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i "$ESP@@1M" disk-images/545/vmlinuz-virt ::/EFI/hype/micro/vmlinuz-virt
mcopy -i "$ESP@@1M" disk-images/545/initramfs-virt ::/EFI/hype/micro/initramfs-virt
mcopy -i "$ESP@@1M" tools/589/hype.cfg ::/hype.cfg
mcopy -i "$ESP@@1M" tools/589/shell-vm0.txt ::/input/vm0.txt

cp /usr/share/edk2/ovmf/OVMF_VARS.fd "$S"/VARS.fd
LOG="$S"/serial.txt
timeout "${TIMEOUT:-300}" qemu-system-x86_64 -machine q35 -m 4096 -nodefaults \
  -accel kvm -cpu host -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file="$S"/VARS.fd \
  -device ich9-ahci,id=ahci \
  -drive format=raw,file="$ESP",if=none,id=d0 \
  -device ide-hd,drive=d0,bus=ahci.0,serial=HYPEESPDISK,bootindex=0 \
  -drive format=raw,file="$S"/target.img,if=none,id=d1 \
  -device ide-hd,drive=d1,bus=ahci.1,serial=HYPE589TGT \
  -serial "file:$LOG" -display none -vga std || true

echo "=== media devices registered (want >= 2: both AHCI ports) ==="
LC_ALL=C grep -a "media: registered host device" "$LOG" | head
echo "=== file resolution (was 'NOT FOUND' before #589) ==="
LC_ALL=C grep -aE "FILE-backed guest disk|NOT FOUND on any" "$LOG" | head
echo "=== guest verdict ==="
LC_ALL=C grep -aE "KBOOT589-42|HYPE589-2ND-PORT|SCRIPT vm0: (PASS|FAIL)" "$LOG" | grep -av "screen|" | head
LC_ALL=C grep -aq "SCRIPT vm0: PASS" "$LOG" && echo "PASS: file on the 2nd AHCI port resolved + read" || { echo "FAIL"; exit 1; }
