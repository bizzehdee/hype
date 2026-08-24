#!/bin/bash
# #715: the vectored-write QEMU leg for the PHYSICAL NVMe backend -- the NVMe twin of
# tools/295/run-295-phys.sh. hype boots off an AHCI ESP disk; a separate QEMU-emulated NVMe
# controller (serial HYPE715SCRATCH) is the guest's PHYSICAL virtio-blk disk, so guest writes go
# guest virtqueue -> drain batching -> blk_phys -> ONE hype_nvme_host_writev() command instead of
# one hype_nvme_host_write() per segment. The DIAG line's vec= counters are the observable:
# vec=0/0/0 here means the vectored path never engaged and the leg FAILS.
#
# Builds with HYPE_M10_6_AUTOCONFIRM=1 (QEMU-scratch only, never a stick build) and `make clean`s
# on both sides, because make ignores EXTRA_CFLAGS changes.
set -e
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-$(mktemp -d)}"
echo "scratch: $S"

killall -9 qemu-system-x86_64 2>/dev/null || true
sleep 1

make clean >/dev/null
make all EXTRA_CFLAGS=-DHYPE_M10_6_AUTOCONFIRM=1 >/dev/null
cp build/hype.efi "$S"/hype-autoconfirm.efi

rm -f "$S"/esp.img "$S"/scratch.img
dd if=/dev/zero of="$S"/esp.img bs=1M count=512 conv=fsync status=none
sfdisk --label gpt -q "$S"/esp.img <<SFDISK
2048,,U
SFDISK
mformat -i "$S"/esp.img@@1M -F ::
mmd -i "$S"/esp.img@@1M ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/input
mcopy -i "$S"/esp.img@@1M "$S"/hype-autoconfirm.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$S"/esp.img@@1M fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i "$S"/esp.img@@1M disk-images/alpine-hype-dbg.iso ::/iso/test.iso
mcopy -i "$S"/esp.img@@1M tools/715/hype-phys.cfg ::/hype.cfg
mcopy -i "$S"/esp.img@@1M tools/715/write-workload-vm0.txt ::/input/vm0.txt

# 64 MiB scratch, poisoned with 0xEE so "zeros landed" is a real observation (same #295 trick).
python3 -c "import sys; sys.stdout.buffer.write(b'\xee'*(64*1024*1024))" > "$S"/scratch.img

for ATTEMPT in 1 2 3 4; do
  cp fw/OVMF_VARS.fd "$S"/VARS.fd
  timeout "${1:-420}" qemu-system-x86_64 \
    -machine q35 -m 3072 -nodefaults \
    -accel kvm -accel tcg -cpu host -smp 2 \
    -drive if=pflash,format=raw,readonly=on,file=fw/OVMF_CODE.fd \
    -drive if=pflash,format=raw,file="$S"/VARS.fd \
    -device ich9-ahci,id=ahci \
    -drive format=raw,file="$S"/esp.img,if=none,id=d0 \
    -device ide-hd,drive=d0,bus=ahci.0,serial=HYPEESPDISK,bootindex=0 \
    -drive format=raw,file="$S"/scratch.img,if=none,id=d1 \
    -device nvme,drive=d1,serial=HYPE715SCRATCH \
    -serial stdio -display none -vga none > "$S"/serial.txt 2>&1 || true
  if grep -aq "hype: build" "$S"/serial.txt; then break; fi
  echo "attempt $ATTEMPT: no hype banner (#371 noboot) -- retrying"
done

# Restore the default build so a later stage.sh cannot pick up the autoconfirm binary.
make clean >/dev/null
make all >/dev/null

echo "=== verdict ==="
grep -a "write-workload-715" "$S"/serial.txt | head -5
echo "=== DIAG ==="
grep -a "BLK WRITE\|VBLK QDEPTH" "$S"/serial.txt | tail -4
echo "=== data landed (first bytes of the dd range must be 00, tail past 48MiB still EE) ==="
head -c 16 "$S"/scratch.img | od -An -tx1 | head -1
dd if="$S"/scratch.img bs=1M skip=50 count=1 status=none | od -An -tx1 | head -1
VEC=$(grep -a "BLK WRITE" "$S"/serial.txt | tail -1 | sed -n 's/.*vec=\([0-9]*\)\/.*/\1/p')
if [ -n "$VEC" ] && [ "$VEC" -gt 0 ]; then echo "PASS: vectored NVMe commands issued: $VEC"; else echo "FAIL: vec counter is zero or missing"; exit 1; fi
