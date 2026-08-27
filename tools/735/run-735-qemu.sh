#!/bin/bash
# #735: a guest `reboot` pinned to a non-BSP vCPU never reaches the reset port.
#
# tools/525's rig already proves the 0xCF9 path from vCPU 1 and PASSES. This rig
# differs from it in ONE deliberate way, which is the difference between that rig
# and the 5950X hardware runs that fail:
#
#   525:  -smp 4 (no SMT)      -> hype grants 2 physical cores -> guest sees 2 CPUs
#   735:  -smp cores=4,threads=2 + topoext -> 2 physical cores -> guest sees 4 CPUs
#
# On the hardware the guest reported CPUS-BEFORE-4-UP-nn on every failing boot,
# because hype hands a vCPU a whole physical core and the core's SMT sibling comes
# free (#564). The 525 rig never had that bonus, so it never had 4 CPUs, and the
# reboot it proves is a 2-CPU reboot. Whether the extra pair is what wedges
# stop_machine is exactly the question.
#
# Everything else is 525's rig: same ISO, same GPT ESP, same USB log volume.
# The input script is the hardware runbook's own reboot-pin (#728 markers), so a
# guest that never restarts prints STALE-SHELL rather than false-passing on the
# previous boot's login prompt still being on screen.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-disk-images/735}"
B=build
SECS="${1:-600}"
SMP="${SMP:--smp cpus=8,cores=4,threads=2,sockets=1}"
CPU="${CPUFLAGS:-host,topoext=on}"
ISO="${ISO:-disk-images/alpine-virt-console.iso}"
CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE.fd}
VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS.fd}
mkdir -p "$S"
killall -9 "$(basename "$QEMU")" 2>/dev/null || true; sleep 1
pgrep -f "[q]emu-system-x86_64" >/dev/null && { echo "FAIL: a qemu is still running"; exit 1; }

rm -f $S/usb.img $S/esp.img $S/serial.txt
dd if=/dev/zero of=$S/usb.img bs=1M count=64 status=none
mkfs.vfat -F 32 -n HYPEUSB $S/usb.img >/dev/null

cat > $S/hype.cfg <<'CFG'
[hype]
config_version = 1

[vm.run1a]
label = run735
vcpus = 2
mem_mb = 1024
boot = installer
install_media = \iso\test.iso
firmware = uefi
os_hint = linux
target_disk = file:\hype\disks\run735.img
CFG

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
mcopy -i "$E" tools/hw-val-2026-08-25/input-1a/vm0.txt ::/input/vm0.txt

cp "$VARS" $S/VARS.fd
timeout "$SECS" "$QEMU" \
  -machine q35 -m 4096 -nodefaults \
  -accel kvm -accel tcg -cpu "$CPU" $SMP \
  -drive if=pflash,format=raw,readonly=on,file="$CODE" \
  -drive if=pflash,format=raw,file=$S/VARS.fd \
  -drive format=raw,file=$S/esp.img,if=none,id=esp \
  -device ide-hd,drive=esp,bus=ide.0,bootindex=0 \
  -device qemu-xhci,id=xhci \
  -drive format=raw,file=$S/usb.img,if=none,id=stick \
  -device usb-storage,bus=xhci.0,drive=stick \
  -serial stdio -display none -vga none > $S/serial.txt 2>&1 || true

echo "=== what the guest saw ==="
grep -aoE 'CPUS-BEFORE-[0-9]+-UP-[0-9]+|CPU-PINNED-[0-9]+|STALE-SHELL|FRESH-BOOT|CPUS-AFTER-[0-9]+-UP-[0-9]+' $S/serial.txt | sort -u || true
echo "=== the reset, if it happened ==="
grep -aE 'guest reset via' $S/serial.txt | head -3 || true
echo "=== verdict ==="
rc=0
if ! grep -aq 'hype: build' $S/serial.txt; then echo "NOBOOT: hype never ran (#371)"; exit 2; fi
if ! grep -aq 'CPUS-BEFORE-' $S/serial.txt; then echo "FAIL: guest never reached a shell"; exit 1; fi
if grep -aq 'STALE-SHELL' $S/serial.txt; then
  echo "REPRODUCED (#735): the guest never restarted -- same shell after 'reboot'"; rc=1
elif grep -aq 'reboot-pin-nonbsp' $S/serial.txt; then
  echo "PASS: one clean restart to login from a non-BSP vCPU"
else
  echo "REPRODUCED (#735): no restart and no stale shell -- the guest wedged after 'reboot'"; rc=1
fi
grep -aoE 'ioio=[0-9]+' $S/serial.txt | tail -2 || true
exit "$rc"
