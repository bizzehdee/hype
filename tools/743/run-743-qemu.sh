#!/bin/bash
# #743: does a HID on a RECYCLED xHCI slot id fail its interrupt-IN transfers?
#
# On the 5950X, four device-instances across two boots said yes with no exceptions: every
# claimed HID that failed cc=4 sat on a slot id previously enabled for another device and
# then disabled, and every one that worked sat on a fresh id. The mechanism was never
# identified -- #734 worked around it by keeping a bounded number of non-HID slots rather
# than handing them straight back.
#
# This rig tries to make QEMU do it, by defeating that workaround on purpose:
#
#   hub port 1..4  four usb-storage devices. The log stick on the root port is already
#                  the medium, so these are inventory only. The keep-budget is 3, so the
#                  FOURTH is released and its slot id goes back.
#   hub port 5     the keyboard, which then inherits the recycled id.
#   hub port 6     the mouse.
#
# A PASS here does NOT clear #743 -- it says QEMU's xHCI model does not reproduce it,
# which is worth knowing (it would mean the fault is in silicon behaviour and the next
# step is hardware instrumentation, not more rig work). A FAIL reproduces it and makes it
# iterable.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
kb_reports() {
  # Sum reports= across every "DIAG: HID[i/n] ... reports=N" line of the LAST dump.
  grep -a "fw-1 DIAG: HID\[.*reports=" "$1" | tail -"$(kb_n "$1")" \
    | sed -n 's/.*reports=\([0-9]*\).*/\1/p' | awk '{t+=$1} END{print (NR?t:"")}'
}
kb_n() {
  # How many keyboards the last dump reported, from the [i/n] field; 1 if absent.
  local n
  n=$(grep -a -o "fw-1 DIAG: HID\[[0-9]*/[0-9]*\]" "$1" | tail -1 | sed -n 's|.*/\([0-9]*\)\]|\1|p')
  echo "${n:-1}"
}
cd "$(git rev-parse --show-toplevel)"
S=rig/i743
SECS="${1:-180}"
mkdir -p $S
rm -f $S/serial.log $S/esp.img $S/usb.img $S/r1.txt

killall -9 "$(basename "$QEMU")" 2>/dev/null || true
sleep 1
pgrep -f "[q]emu-system-x86_64" >/dev/null && { echo "FAIL: a qemu is still running"; exit 1; }

dd if=/dev/zero of=$S/usb.img bs=1M count=64 status=none
for f in 1 2 3 4 5 6; do
  dd if=/dev/zero of=$S/f$f.img bs=1M count=4 status=none
  mkfs.vfat -F 12 -n FILLER$f $S/f$f.img >/dev/null 2>&1 || true
done
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
    grep -aq "fw-1 DIAG: HID\[.*reports=" $S/serial.log 2>/dev/null && break
  done
  for _ in $(seq 1 60); do
    printf 'sendkey a\n'; sleep 0.2
    printf 'mouse_move 5 5\n'; sleep 0.2
  done
  # Let a DIAG line print with phase 1's totals in it, then freeze the mouse.
  sleep 40
  kb_reports $S/serial.log > $S/r1.txt
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
  -device usb-hub,id=hub1,bus=xhci.0,port=2,ports=8 \
  -drive format=raw,file=$S/f1.img,if=none,id=f1 \
  -device usb-storage,bus=xhci.0,port=2.1,drive=f1,serial=RECYCLE1 \
  -drive format=raw,file=$S/f2.img,if=none,id=f2 \
  -device usb-storage,bus=xhci.0,port=2.2,drive=f2,serial=RECYCLE2 \
  -drive format=raw,file=$S/f3.img,if=none,id=f3 \
  -device usb-storage,bus=xhci.0,port=2.3,drive=f3,serial=RECYCLE3 \
  -drive format=raw,file=$S/f4.img,if=none,id=f4 \
  -device usb-storage,bus=xhci.0,port=2.4,drive=f4,serial=RECYCLE4 \
  -drive format=raw,file=$S/f5.img,if=none,id=f5 \
  -device usb-storage,bus=xhci.0,port=2.5,drive=f5,serial=RECYCLE5 \
  -drive format=raw,file=$S/f6.img,if=none,id=f6 \
  -device usb-storage,bus=xhci.0,port=2.6,drive=f6,serial=RECYCLE6 \
  -device usb-kbd,bus=xhci.0,port=2.7 \
  -device usb-mouse,bus=xhci.0,port=2.8 \
  -serial "file:$S/serial.log" -monitor stdio -display none -vga std >$S/mon.log 2>$S/qemu.err || true

echo "=== claims"
grep -a "host-hid: USB .* CLAIMED\|no interrupt-IN block free" $S/serial.log | head -4
echo "=== slot recycling (#743/#734)"
grep -a "RECYCLED\|Disable Slot\|keep budget spent" $S/serial.log | head -12
echo "=== did a CLAIMED HID land on a recycled id?"
for sl in $(grep -a -o "host-hid: USB .* slot[0-9]*" $S/serial.log | sed -n 's/.*slot\([0-9]*\)/\1/p' | sort -u); do
  if grep -aq "Enable Slot: $sl -- RECYCLED" $S/serial.log; then
    echo "  slot $sl: a claimed HID IS on a recycled id -- this is the #743 condition"
  else
    echo "  slot $sl: fresh"
  fi
done
echo "=== transfer errors, if any"
grep -a "interrupt-IN transfer FAILED" $S/serial.log | head -4
echo "=== last counters"
grep -a "fw-1 DIAG: HID\[\|fw-1 DIAG: MOUSE\|fw-1 DIAG: host-kbd" $S/serial.log | tail -5

fail=0
say() { echo "$1"; fail=1; }
grep -aq "host-hid: USB keyboard CLAIMED" $S/serial.log || say "FAIL: the keyboard was not claimed"
grep -aq "host-hid: USB mouse CLAIMED" $S/serial.log || say "FAIL: the mouse was not claimed"
kb=$(kb_reports $S/serial.log)
r1=$(cat $S/r1.txt 2>/dev/null)
echo "keyboard reports: ${r1:-none} at the end of phase 1 -> ${kb:-none} at the end of phase 2"
[ "${kb:-0}" -gt 0 ] 2>/dev/null || say "FAIL: the keyboard never reported at all"
[ -n "$r1" ] || say "FAIL: no phase-1 count captured -- the run did not reach phase 2"
[ "${kb:-0}" -gt "${r1:-0}" ] 2>/dev/null ||
  say "FAIL: the keyboard stopped reporting once the mouse went idle [#734]"
[ $fail -eq 0 ] && echo "ALL PASS: an idle claimed mouse does not silence the keyboard"
exit $fail
