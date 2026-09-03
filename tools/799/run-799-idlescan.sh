#!/bin/bash
# #799: measure what an ARMED-BUT-UNMATCHABLE input script costs the host loop.
#
# The AMD-L0 hardware boot found HOUSECOST section 5 -- the input-script screen scan -- taking
# 38.7 ms per scan at 10 Hz, 95% of all loop housekeeping and 37% of wall time, because the
# script's first `expect` could not match while the guest was silent. Every other rig in the
# tree hides it: their scripts COMPLETE, and hype_input_runner_scan() returns immediately once
# r->done is set, so the cost stops after a minute and reads as noise.
#
# This rig arms a script whose `expect` can NEVER match, which is the failing state as a
# deterministic, sandboxed reproduction. The verdict is HOUSECOST s5 -- nothing else.
#
# `-vga std`, NOT `-vga none`: the scan copies rows x cols cells, so the cost is proportional to
# the terminal grid, and with no GOP the grid is tiny. Measured on the first version of this rig
# with `-vga none`: s5=704ms over 180 s (0.39 ms per scan) against the hardware boot's 38.7 ms.
# The sandbox cannot match hardware's absolute cost either way -- it reproduces the SHAPE, and
# `-vga std` at least gets the grid into the right order of magnitude.
#
#   tools/799/run-799-idlescan.sh [seconds]        default 200
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-disk-images/799}"
B=build
SECS="${1:-200}"
ISO="${ISO:-disk-images/alpine-virt-console.iso}"
CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE.fd}
VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS.fd}
mkdir -p "$S"
killall -9 "$(basename "$QEMU")" 2>/dev/null || true; sleep 1

rm -f $S/usb.img $S/esp.img
dd if=/dev/zero of=$S/usb.img bs=1M count=64 status=none
mkfs.vfat -F 32 -n HYPEUSB $S/usb.img >/dev/null

cat > $S/hype.cfg <<'CFG'
[hype]
config_version = 1

[vm.idlescan]
label = idlescan799
vcpus = 2
mem_mb = 1024
boot = installer
install_media = \iso\test.iso
firmware = uefi
os_hint = linux
target_disk = file:\hype\disks\smp.img
CFG

# The whole point: an expect no guest will ever print, so the script stays armed for the
# entire run and the pc never moves off it.
cat > $S/vm0.txt <<'IN'
timeout 100000
expect HYPE-799-NEVER-PRINTED-BY-ANY-GUEST
IN

# GPT, not superfloppy -- a bare-FAT image kills the media resolver (tools/525 explains).
dd if=/dev/zero of=$S/esp.img bs=1M count=512 status=none
sfdisk -q $S/esp.img <<'PT'
label: gpt
start=2048, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B
PT
E="$S/esp.img@@1M"
mkfs.vfat -F 32 -n HYPEESP --offset 2048 $S/esp.img >/dev/null
mmd -i "$E" ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/input ::/hype ::/hype/disks
mcopy -i "$E" $B/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$E" fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i "$E" "$ISO" ::/iso/test.iso
mcopy -i "$E" $S/hype.cfg ::/hype.cfg
mcopy -i "$E" $S/vm0.txt ::/input/vm0.txt

for ATTEMPT in 1 2 3; do
  cp "$VARS" $S/VARS.fd
  timeout "$SECS" "$QEMU" \
    -machine q35 -m 4096 -nodefaults \
    -accel kvm -accel tcg -cpu host -smp 4 \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file=$S/VARS.fd \
    -drive format=raw,file=$S/esp.img,if=none,id=esp \
    -device ide-hd,drive=esp,bus=ide.0,bootindex=0 \
    -device qemu-xhci,id=xhci \
    -drive format=raw,file=$S/usb.img,if=none,id=stick \
    -device usb-storage,bus=xhci.0,drive=stick \
    -serial stdio -display none -vga std > $S/serial-$ATTEMPT.txt 2>&1 || true
  if grep -aq "hype: build" $S/serial-$ATTEMPT.txt; then
    echo "attempt $ATTEMPT: hype ran"; cp $S/serial-$ATTEMPT.txt $S/serial.txt; break
  fi
  echo "attempt $ATTEMPT: hype never ran (#371 noboot) -- retrying"
done

echo "=== verdict ==="
# The script must still be armed and unmatched -- otherwise the rig measured nothing.
if grep -aq "SCRIPT vm0: PASS" $S/serial.txt; then
  echo "INVALID: the script PASSED, so it was not unmatchable -- this rig measured nothing"; exit 2
fi
grep -a "SCRIPT vm0: armed" $S/serial.txt | tail -1
S5=$(grep -a "HOUSECOST vm0:" $S/serial.txt | tail -1 | grep -o "s5=[0-9]*ms")
HOUSE=$(grep -a "LOOPPHASE" $S/serial.txt | tail -1 | grep -o "house=[0-9]*ms")
ITERS=$(grep -a "DRAIN: iters=" $S/serial.txt | tail -1 | grep -o "iters=[0-9]*")
echo "final: $S5  $HOUSE  $ITERS"
grep -a "HOUSECOST vm0:" $S/serial.txt | tail -1
grep -a "LOOPPHASE" $S/serial.txt | tail -1
