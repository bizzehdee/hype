#!/bin/bash
# #295: the vectored-write QEMU leg. hype boots off SATA port 0; a second SATA disk
# (serial HYPE295SCRATCH) is the guest's PHYSICAL virtio-blk disk, so guest writes go
# guest virtqueue -> drain batching -> blk_phys -> ONE multi-PRDT AHCI command against
# QEMU's ich9-ahci. The DIAG line's vec= counters are the observable: vec=0/0/0 here
# means the merge never engaged and the leg FAILS.
#
# Builds with HYPE_M10_6_AUTOCONFIRM=1 (QEMU-scratch only, never a stick build) and
# `make clean`s on BOTH sides, because make ignores EXTRA_CFLAGS changes.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-$(mktemp -d /mnt/data/dev/hype/disk-images/rig586.XXXXXX)}"
echo "scratch: $S"

killall -9 "$(basename "$QEMU")" 2>/dev/null || true
sleep 1

make clean >/dev/null
make all EXTRA_CFLAGS=-DHYPE_M10_6_AUTOCONFIRM=1 >/dev/null
# Snapshot the binary IMMEDIATELY: build/ is shared state, and anything else building in this
# repo between here and the mcopy would stage a different (or torn) binary.
cp build/hype.efi "$S"/hype-autoconfirm.efi

rm -f "$S"/esp.img "$S"/scratch.img
# GPT-partitioned, NOT a bare FAT superfloppy: hype's media resolver locates the volume with
# hype_gpt_find_partition() before handing it to core/fat.c, so a partitionless image means
# \iso\test.iso is never found and the guest sees "No bootable option" (run-guest.sh's
# build_esp_file() carries the same scar in its own comments).
dd if=/dev/zero of="$S"/esp.img bs=1M count=512 conv=fsync status=none
sfdisk --label gpt -q "$S"/esp.img <<SFDISK
2048,,U
SFDISK
mformat -i "$S"/esp.img@@1M -F ::
mmd -i "$S"/esp.img@@1M ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/input
mcopy -i "$S"/esp.img@@1M "$S"/hype-autoconfirm.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$S"/esp.img@@1M fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i "$S"/esp.img@@1M disk-images/alpine-hype-dbg.iso ::/iso/test.iso
mcopy -i "$S"/esp.img@@1M tools/586/hype.cfg ::/hype.cfg
mcopy -i "$S"/esp.img@@1M tools/295/write-workload-vm0.txt ::/input/vm0.txt

# 64 MiB scratch, poisoned with 0xEE so "zeros landed" is a real observation.
# (a pipe into dd under-fills with bs=1M -- partial reads count as blocks -- so build the
#  poison from a real file)
python3 -c "import sys; sys.stdout.buffer.write(b'\xee'*(64*1024*1024))" > "$S"/scratch.img

for ATTEMPT in 1 2 3 4; do
  cp fw/OVMF_VARS.fd "$S"/VARS.fd
  timeout "${1:-420}" "$QEMU" \
    -machine q35 -m 3072 -nodefaults \
    -accel kvm -cpu host -smp 2 \
    -drive if=pflash,format=raw,readonly=on,file=fw/OVMF_CODE.fd \
    -drive if=pflash,format=raw,file="$S"/VARS.fd \
    -device ich9-ahci,id=ahci \
    -drive format=raw,file="$S"/esp.img,if=none,id=d0 \
    -device ide-hd,drive=d0,bus=ahci.0,serial=HYPEESPDISK,bootindex=0 \
    -drive format=raw,file="$S"/scratch.img,if=none,id=d1 \
    -device ide-hd,drive=d1,bus=ahci.1,serial=HYPE295SCRATCH \
    -serial stdio -display none -vga none > "$S"/serial.txt 2>&1 || true
  if grep -aq "hype: build" "$S"/serial.txt; then break; fi
  echo "attempt $ATTEMPT: no hype banner (#371 noboot) -- retrying"
done

# Restore the default build so a later stage.sh cannot pick up the autoconfirm binary.
make clean >/dev/null
make all >/dev/null

echo "=== #586: did the [disk.*] backing=physical entry bind? ==="
grep -a "586.*-> target\|PHYSICAL AHCI/SATA backend\|PHYSICAL USB" "$S"/serial.txt | head -3
grep -aq "PHYSICAL AHCI/SATA backend" "$S"/serial.txt || { echo "FAIL: section-form physical target did not attach"; exit 1; }
echo "=== verdict ==="
grep -a "INPUT.*pass\|input.*PASS\|write-workload" "$S"/serial.txt | head -5
echo "=== DIAG ==="
grep -a "BLK WRITE\|VBLK QDEPTH" "$S"/serial.txt | tail -4
echo "=== data landed (first bytes of the dd range must be 00, tail past 48MiB still EE) ==="
head -c 16 "$S"/scratch.img | od -An -tx1 | head -1
dd if="$S"/scratch.img bs=1M skip=50 count=1 status=none | od -An -tx1 | head -1
VEC=$(grep -a "BLK WRITE" "$S"/serial.txt | tail -1 | sed -n 's/.*vec=\([0-9]*\)\/.*/\1/p')
if [ -n "$VEC" ] && [ "$VEC" -gt 0 ]; then echo "PASS: vectored commands issued: $VEC"; else echo "FAIL: vec counter is zero or missing"; exit 1; fi
