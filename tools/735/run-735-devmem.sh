#!/bin/bash
# #735/#749: KEPT AS A RECORD OF A DEAD END. This rig does not work and cannot be made to.
#
# The aim was to make a guest AP touch an address no device claims, on demand, so the AP
# call site of #749's fix could be proven in QEMU. Four attempts, ten minutes each:
#
#   devmem 0x100000004 32          -> 127  (busybox applet, not symlinked)
#   taskset -c 1 ...               -> 127  (util-linux, not on the live ISO)
#   busybox devmem ...             -> 127  (not compiled into Alpine's busybox)
#   dd if=/dev/mem skip=4294967300 -> 1    (dd ran; the KERNEL refused it)
#
# The last is the answer: Alpine builds with CONFIG_STRICT_DEVMEM, so a Linux guest cannot
# be made to touch unclaimed physical memory from userspace. That is Linux working as
# designed, not a gap in the rig. Getting past it needs `iomem=relaxed` on the guest
# cmdline, and this guest boots an ISO through GRUB, where hype's `cmdline` key does not
# reach.
#
# What DOES prove the absorber is tests/micro/unclaimed.c (tools/749) -- but on the BSP,
# because the microtest harness is single-vCPU. Proving the AP call site needs either an
# SMP microtest with its own AP trampoline, or the hardware, where the pre-fix counter
# already read unhandled=37009095 on vCPU 1.
#
# Left runnable, because if a guest with `iomem=relaxed` ever exists in this tree, this is
# the script to point at it.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-disk-images/735b}"
B=build
SECS="${1:-600}"
SMP="${SMP:--smp cpus=8,cores=4,threads=2,sockets=1}"
# MEDIA=usb puts EVERYTHING on one emulated USB device -- hype boots from it,
# streams the ISO from it and writes its log to it, which is the 5950X's own
# shape (one drive, hype on partition 1, the ISO on partition 2). Every other
# rig in this tree serves the ESP over IDE and uses USB only for the log, so
# the whole boot+media+log path shares no lock and no controller with itself.
# That is the largest remaining difference between the rigs that PASS and the
# hardware that does not, so it is a switch rather than a second file.
MEDIA="${MEDIA:-ide}"
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
mcopy -i "$E" tools/hw-val-2026-08-25/input-1a-735/vm0-devmem.txt ::/input/vm0.txt

if [ "$MEDIA" = usb ]; then
  ESPDEV=(-drive format=raw,file=$S/esp.img,if=none,id=esp
          -device usb-storage,bus=xhci.0,drive=esp,bootindex=0,serial=HYPE735ESP)
else
  ESPDEV=(-drive format=raw,file=$S/esp.img,if=none,id=esp
          -device ide-hd,drive=esp,bus=ide.0,bootindex=0)
fi
echo "media path: $MEDIA (esp over ${MEDIA})"
cp "$VARS" $S/VARS.fd
timeout "$SECS" "$QEMU" \
  -machine q35 -m 4096 -nodefaults \
  -accel kvm -accel tcg -cpu "$CPU" $SMP \
  -drive if=pflash,format=raw,readonly=on,file="$CODE" \
  -drive if=pflash,format=raw,file=$S/VARS.fd \
  -device qemu-xhci,id=xhci \
  "${ESPDEV[@]}" \
  -drive format=raw,file=$S/usb.img,if=none,id=stick \
  -device usb-storage,bus=xhci.0,drive=stick,serial=HYPE735LOG \
  -serial stdio -display none -vga none > $S/serial.txt 2>&1 || true

echo "=== the unclaimed accesses, from the AP ==="
grep -aoE 'DEV1-[0-9]+|DEV2-[0-9]+|DEVMEM-100-DONE|AP-STILL-ALIVE-[0-9]+' $S/serial.txt | sort -u || true
echo "=== hype on those accesses ==="
grep -a "claimed by NO device" $S/serial.txt | head -3 || true
for v in 1 2 3; do
  grep -a "APVCPU vm0/$v" $S/serial.txt | tail -1 \
    | sed -n "s/.*\(exits=[0-9]* unhandled=[0-9]* unclaimed=[0-9]*\).*/  vCPU $v \1/p" || true
done
echo "=== what the guest saw ==="
grep -aoE 'CPUS-BEFORE-[0-9]+-UP-[0-9]+|CPU-PINNED-[0-9]+|STALE-SHELL|FRESH-BOOT|CPUS-AFTER-[0-9]+-UP-[0-9]+' $S/serial.txt | sort -u || true
echo "=== the reset, if it happened ==="
grep -aE 'guest reset via' $S/serial.txt | head -3 || true
echo "=== verdict ==="
rc=0
if ! grep -aq 'hype: build' $S/serial.txt; then echo "NOBOOT: hype never ran (#371)"; exit 2; fi
# A refused script costs the whole run and otherwise reads as "the guest never reached a
# shell", which sent one run down entirely the wrong path. Name it.
if grep -aq 'PARSE ERROR' $S/serial.txt; then
  grep -a 'PARSE ERROR' $S/serial.txt | head -1
  echo "FAIL: the input script was REFUSED, so nothing was typed -- fix the script, not hype"
  exit 1
fi
if ! grep -aq 'CPUS-BEFORE-' $S/serial.txt; then echo "FAIL: guest never reached a shell"; exit 1; fi
if grep -aqE 'DEV1-(1[0-9]+|[1-9])' $S/serial.txt; then
  echo "FAIL: devmem itself failed (non-zero exit) -- no unclaimed access was made, so this"
  echo "      run proves nothing about #735. Fix the guest command, not hype."
  rc=1
elif ! grep -aq 'DEV1-0' $S/serial.txt; then
  echo "REPRODUCED (#735 root cause): the AP never returned from ONE unclaimed access."
  echo "  That is the 37-million-NPF spin, and it is why the pinned reboot never ran."
  rc=1
elif ! grep -aq 'DEVMEM-100-DONE' $S/serial.txt; then
  echo "FAIL: one unclaimed access completed but a hundred did not"; rc=1
elif grep -aq 'STALE-SHELL' $S/serial.txt; then
  echo "REPRODUCED (#735): the guest never restarted -- same shell after 'reboot'"; rc=1
elif grep -aq '735-unclaimed-then-reboot-on-the-same-ap' $S/serial.txt; then
  echo "PASS: the AP absorbed 100+ unclaimed accesses and then completed a pinned reboot"
else
  echo "REPRODUCED (#735): no restart and no stale shell -- the guest wedged after 'reboot'"; rc=1
fi
grep -aoE 'ioio=[0-9]+' $S/serial.txt | tail -2 || true
# The counters are the before/after. Hardware boot 6 read unhandled=37009095 unclaimed=0
# (the field did not exist); a fixed build must show unclaimed climbing and unhandled flat.
# THE assertion this rig exists for: an AP absorbed unclaimed accesses instead of spinning.
# Summed across vCPU 1..3 because nothing pins the access -- which CPU services it is the
# scheduler's business, and hype's counters are what say an AP did.
uc=0; uh=0
for v in 1 2 3; do
  a=$(grep -a "APVCPU vm0/$v" $S/serial.txt | tail -1 | sed -n 's/.*unclaimed=\([0-9]*\).*/\1/p')
  b=$(grep -a "APVCPU vm0/$v" $S/serial.txt | tail -1 | sed -n 's/.*unhandled=\([0-9]*\).*/\1/p')
  uc=$((uc + ${a:-0})); uh=$((uh + ${b:-0}))
done
echo "AP totals across vCPU 1-3: unclaimed=$uc unhandled=$uh"
echo "  (hardware boot 6, pre-fix: vCPU 1 alone read unhandled=37009095 unclaimed=0)"
if [ "$uc" -lt 1 ]; then
  echo "FAIL: no AP absorbed a single unclaimed access -- the AP call site is UNPROVEN by"
  echo "      this run, whatever else passed"; rc=1
else
  echo "PROVEN: an AP took $uc unclaimed accesses, absorbed every one, and kept running [#749]"
fi
if [ "$uh" -gt 1000 ]; then
  echo "FAIL: unhandled=$uh on the APs -- something is still NOT being absorbed"; rc=1
fi
exit "$rc"
