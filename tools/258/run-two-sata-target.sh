#!/bin/bash
# #258: two SATA disks on ONE ich9-ahci. hype boots from the ESP on port 0; the config names the
# scratch on port 1 as its physical target -- the exact layout the ticket was filed about.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-$(mktemp -d)}"
cp "$(dirname "$(readlink -f "$0")")/hype.cfg" "$S/hype.cfg"

rm -f $S/esp.img $S/scratch.img
dd if=/dev/zero of=$S/esp.img bs=1M count=512 status=none
mkfs.vfat -F 32 -n HYPEESP $S/esp.img >/dev/null
mmd -i $S/esp.img ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso
mcopy -i $S/esp.img build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i $S/esp.img fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i $S/esp.img disk-images/alpine-hype-dbg.iso ::/iso/test.iso
mcopy -i $S/esp.img $S/hype.cfg ::/hype.cfg

dd if=/dev/zero of=$S/scratch.img bs=1M count=64 status=none

for ATTEMPT in 1 2 3 4; do
cp /usr/share/OVMF/OVMF_VARS.fd $S/VARS.fd
timeout "${1:-150}" "$QEMU" \
  -machine q35 -m 2048 -nodefaults \
  -accel kvm -accel tcg -cpu host -smp 2 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=$S/VARS.fd \
  -device ich9-ahci,id=ahci \
  -drive format=raw,file=$S/esp.img,if=none,id=d0 \
  -device ide-hd,drive=d0,bus=ahci.0,serial=HYPEESPDISK,bootindex=0 \
  -drive format=raw,file=$S/scratch.img,if=none,id=d1 \
  -device ide-hd,drive=d1,bus=ahci.1,serial=HYPE228SCRATCH \
  -serial stdio -display none -vga none > $S/serial.txt 2>&1 || true

  if grep -aq "host-ahci\|host-disk:" $S/serial.txt; then
    echo "attempt $ATTEMPT: hype ran"; break
  fi
  echo "attempt $ATTEMPT: hype never ran (BdsDxe flake) -- retrying"
done

echo "=== inventory + selection ==="
grep -a "258\|host-disk:\|phys-write:" $S/serial.txt | head -30
