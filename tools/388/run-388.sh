#!/bin/bash
# #388: USB physical: target. Leg 1: the SECOND stick is the confirmed target; guest dd lands
# on stickB.img byte-exact and stick A (the boot/log medium) stays untouched at that LBA.
# Leg 2: naming the BOOT medium's serial must refuse to arm (rule 3, by identity).
set -e
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-$(mktemp -d /mnt/data/dev/hype/disk-images/rig388.XXXXXX)}"
echo "scratch: $S"
killall -9 qemu-system-x86_64 2>/dev/null || true
sleep 1

make clean >/dev/null && make all EXTRA_CFLAGS=-DHYPE_M10_6_AUTOCONFIRM=1 >/dev/null
cp build/hype.efi "$S"/hype-ac.efi
make clean >/dev/null && make all >/dev/null   # restore the default build immediately

dd if=/dev/zero of="$S"/stickA.img bs=1M count=64 conv=fsync status=none
mkfs.vfat -F 32 -n HYPEUSBA "$S"/stickA.img >/dev/null
# stick B: raw + poisoned; a physical target needs no filesystem
python3 -c "import sys; sys.stdout.buffer.write(b'\xEE'*(64*1024*1024))" > "$S"/stickB.img

build_esp() { # $1 = cfg
    rm -f "$S"/esp.img
    dd if=/dev/zero of="$S"/esp.img bs=1M count=512 conv=fsync status=none
    sfdisk --label gpt -q "$S"/esp.img <<SFDISK
2048,,U
SFDISK
    mformat -i "$S"/esp.img@@1M -F ::
    mmd -i "$S"/esp.img@@1M ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/input ::/hype ::/hype/disks
    mcopy -i "$S"/esp.img@@1M "$S"/hype-ac.efi ::/EFI/BOOT/BOOTX64.EFI
    mcopy -i "$S"/esp.img@@1M fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
    mcopy -i "$S"/esp.img@@1M disk-images/alpine-hype-dbg.iso ::/iso/test.iso
    mcopy -i "$S"/esp.img@@1M "$1" ::/hype.cfg
    mcopy -i "$S"/esp.img@@1M tools/388/write-vm0.txt ::/input/vm0.txt
}

run_qemu() { # $1 = log, $2 = seconds, $3 = stickB serial
    cp /usr/share/edk2/ovmf/OVMF_VARS.fd "$S"/VARS.fd
    timeout "$2" qemu-system-x86_64 -machine q35 -m 4096 -nodefaults \
      -accel kvm -cpu host -smp 4 \
      -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
      -drive if=pflash,format=raw,file="$S"/VARS.fd \
      -device ich9-ahci,id=ahci \
      -drive format=raw,file="$S"/esp.img,if=none,id=d0 \
      -device ide-hd,drive=d0,bus=ahci.0,bootindex=0 \
      -device qemu-xhci,id=xhci \
      -drive format=raw,file="$S"/stickA.img,if=none,id=ua \
      -device usb-storage,bus=xhci.0,drive=ua,serial=HYPE388BOOT \
      -drive format=raw,file="$S"/stickB.img,if=none,id=ub \
      -device usb-storage,bus=xhci.0,drive=ub,serial="$3" \
      -serial "file:$1" -display none -vga none || true
}

echo "=== leg 1: the second stick as the confirmed target ==="
build_esp tools/388/hype.cfg
run_qemu "$S"/leg1.log 420 HYPE388TGT
LC_ALL=C grep -a "post-config arm for USB\|AUTO-CONFIRMED\|PHYSICAL USB backend\|SCRIPT vm0: PASS\|SCRIPT vm0: FAIL" "$S"/leg1.log | head -6
LC_ALL=C grep -aq "PHYSICAL USB backend (sn 'HYPE388TGT'" "$S"/leg1.log || { echo "FAIL: no USB attach"; exit 1; }
LC_ALL=C grep -aq "SCRIPT vm0: PASS" "$S"/leg1.log || { echo "FAIL: guest write leg"; exit 1; }
MB=$(dd if="$S"/stickB.img bs=512 skip=4096 count=1 status=none | head -c 13)
[ "$MB" = "HYPE388MARKER" ] || { echo "FAIL: marker not on stick B (got '$MB')"; exit 1; }
MA=$(dd if="$S"/stickA.img bs=512 skip=4096 count=1 status=none | head -c 13)
[ "$MA" = "HYPE388MARKER" ] && { echo "FAIL: marker leaked onto stick A"; exit 1; }
echo "leg 1 PASS: marker on B, absent from A"

echo "=== leg 2: naming the boot medium must refuse ==="
build_esp tools/388/hype-bootmedium.cfg
python3 -c "import sys; sys.stdout.buffer.write(b'\xEE'*(64*1024*1024))" > "$S"/stickB.img
run_qemu "$S"/leg2.log 180 HYPE388TGT
LC_ALL=C grep -a "post-config arm\|PHYSICAL USB backend\|not armed" "$S"/leg2.log | head -4
LC_ALL=C grep -aq "PHYSICAL USB backend" "$S"/leg2.log && { echo "FAIL: the boot medium attached as a target"; exit 1; }
echo "leg 2 PASS: the boot medium's serial never armed"
echo "ALL PASS"
