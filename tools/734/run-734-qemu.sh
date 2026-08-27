#!/bin/bash
# #734: an IDLE mouse must not silence the keyboard.
#
# The topology is tools/737's -- a keyboard and a mouse behind a hub on ONE xHCI -- with
# one deliberate difference: this rig NEVER moves the mouse. That is the desktop
# condition the 2026-08-27 5950X boot ran in, and it is what tools/737 cannot show,
# because it drives both devices and a device that reports frequently keeps handing the
# shared state back.
#
# Before the fix, the two claimed HIDs shared ONE interrupt-IN ring and ONE "transfer
# outstanding" flag per controller. An idle input device holds that flag forever, so the
# other endpoint's doorbell was never rung again: measured on the 5950X as
# "HID polls=20745 reports=0" alongside "MOUSE polls=238463 reports=0".
#
# Unlike #736/#737, QEMU CAN prove this one: the fault is in hype's own bookkeeping, not
# in a controller behaviour QEMU does not model.
#
# The greps below anchor on "DIAG: HID polls=", not "DIAG: HID". #734's modseen line also
# starts "fw-1 DIAG: HID" and carries no reports= field, so the looser pattern selected it
# with tail -1 and reported a keyboard that had delivered 320 reports as never having
# reported at all. A rig that fails on a diagnostic being ADDED is worse than no rig.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S=rig/i734
SECS="${1:-180}"
mkdir -p $S
rm -f $S/serial.log $S/esp.img $S/usb.img $S/r1.txt

killall -9 "$(basename "$QEMU")" 2>/dev/null || true
sleep 1
pgrep -f "[q]emu-system-x86_64" >/dev/null && { echo "FAIL: a qemu is still running"; exit 1; }

dd if=/dev/zero of=$S/usb.img bs=1M count=64 status=none
mkfs.vfat -F 32 -n HYPEUSB $S/usb.img >/dev/null

dd if=/dev/zero of=$S/esp.img bs=1M count=512 status=none
mkfs.vfat -F 32 -n HYPEESP $S/esp.img >/dev/null
mmd -i $S/esp.img ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/hype ::/hype/disks
mcopy -i $S/esp.img build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i $S/esp.img fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i $S/esp.img disk-images/alpine-virt-console.iso ::/iso/test.iso
mcopy -i $S/esp.img tools/734/hype.cfg ::/hype.cfg

cp /usr/share/edk2/ovmf/OVMF_VARS.fd $S/VARS.fd

# TWO PHASES, and the second one is the test.
#
#   phase 1: type AND move the mouse -- both endpoints active, which is all tools/737
#            ever did, and which passes with or without the fix.
#   phase 2: the mouse goes STILL and the typing continues.
#
# The pass condition is the keyboard's report count RISING across phase 2. Before the
# fix, the moment the idle mouse won the shared "transfer outstanding" flag it never gave
# it back, so the keyboard's count froze -- exactly the 5950X's HID reports=0.
(
  for _ in $(seq 1 "$SECS"); do
    sleep 1
    grep -aq "fw-1 DIAG: HID polls=" $S/serial.log 2>/dev/null && break
  done
  for _ in $(seq 1 60); do
    printf 'sendkey a\n'; sleep 0.2
    printf 'mouse_move 5 5\n'; sleep 0.2
  done
  # Let a DIAG line print with phase 1's totals in it, then freeze the mouse.
  sleep 40
  grep -a "fw-1 DIAG: HID polls=" $S/serial.log | tail -1 | sed -n 's/.*reports=\([0-9]*\).*/\1/p' > $S/r1.txt
  for _ in $(seq 1 100); do
    printf 'sendkey b\n'; sleep 0.4
  done
  # The DIAG counters print on a ~35s cadence; quitting straight after the keystrokes
  # leaves only phase 1's line in the log.
  sleep 40
  printf 'quit\n'
) | timeout "$SECS" "$QEMU" \
  -machine q35 -m 4096 -nodefaults \
  -accel kvm -cpu host,topoext=on -smp cpus=4,sockets=1,cores=2,threads=2 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=$S/VARS.fd \
  -drive format=raw,file=$S/esp.img,if=none,id=esp \
  -device ide-hd,drive=esp,bus=ide.0,bootindex=0 \
  -device qemu-xhci,id=xhci \
  -drive format=raw,file=$S/usb.img,if=none,id=stick \
  -device usb-storage,bus=xhci.0,port=1,drive=stick,serial=HYPE734USB \
  -device usb-hub,id=hub1,bus=xhci.0,port=2,ports=4 \
  -device usb-kbd,bus=xhci.0,port=2.2 \
  -device usb-mouse,bus=xhci.0,port=2.4 \
  -serial "file:$S/serial.log" -monitor stdio -display none -vga std >$S/mon.log 2>$S/qemu.err || true

echo "=== claims"
grep -a "host-hid: USB .* CLAIMED\|no interrupt-IN block free" $S/serial.log | head -4
echo "=== transfer errors, if any"
grep -a "interrupt-IN transfer FAILED" $S/serial.log | head -4
echo "=== last counters"
grep -a "fw-1 DIAG: HID polls=\|fw-1 DIAG: MOUSE\|fw-1 DIAG: HID modseen=" $S/serial.log | tail -3

fail=0
say() { echo "$1"; fail=1; }
grep -aq "host-hid: USB keyboard CLAIMED" $S/serial.log || say "FAIL: the keyboard was not claimed"
grep -aq "host-hid: USB mouse CLAIMED" $S/serial.log || say "FAIL: the mouse was not claimed"
kb=$(grep -a "fw-1 DIAG: HID polls=" $S/serial.log | tail -1 | sed -n 's/.*reports=\([0-9]*\).*/\1/p')
r1=$(cat $S/r1.txt 2>/dev/null)
echo "keyboard reports: ${r1:-none} at the end of phase 1 -> ${kb:-none} at the end of phase 2"
[ "${kb:-0}" -gt 0 ] 2>/dev/null || say "FAIL: the keyboard never reported at all"
[ -n "$r1" ] || say "FAIL: no phase-1 count captured -- the run did not reach phase 2"
[ "${kb:-0}" -gt "${r1:-0}" ] 2>/dev/null ||
  say "FAIL: the keyboard stopped reporting once the mouse went idle [#734]"
[ $fail -eq 0 ] && echo "ALL PASS: an idle claimed mouse does not silence the keyboard"
exit $fail
