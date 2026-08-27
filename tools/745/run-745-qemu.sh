#!/bin/bash
# #745: a keyboard plugged in AFTER boot must work, with no reboot.
#
# Before this, hype enumerated once at boot and never looked at a port again. There was no
# Port Status Change Event handling anywhere -- core/xhci.h named the event type and
# nothing consumed it. Measured on the 2026-08-27 boot 6: the operator unplugged the
# keyboard, both HIDs took cc=4, #734's recovery backed off correctly, and reports= stayed
# frozen for the rest of the run because nothing ever looked at that port again.
#
# Builds on #744, which notices the departure. This one runs the full cycle on a ROOT PORT:
#
#   phase 1  type on the keyboard, confirm reports
#   phase 2  device_del  -- it departs, #744 releases its slot
#   phase 3  device_add  -- it arrives, and must be enumerated and claimed with no reboot
#   phase 4  type again  -- the reports must climb from where they were
#
# `device_del` and `device_add` are real USB disconnect/connect: the controller raises Port
# Status Change Events exactly as hardware does.
#
# The mouse stays behind the hub throughout as the control -- neither the departure nor the
# arrival may disturb it.
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
S=rig/i745
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
    grep -aq "fw-1 DIAG: HID\[.*reports=" $S/serial.log 2>/dev/null && break
  done
  for _ in $(seq 1 60); do
    printf 'sendkey a\n'; sleep 0.2
    printf 'mouse_move 5 5\n'; sleep 0.2
  done
  # Let a DIAG line print with phase 1's totals in it.
  sleep 40
  kb_reports $S/serial.log > $S/r1.txt
  # PHASE 2: unplug the keyboard. A real USB disconnect -- the controller raises a Port
  # Status Change Event, which is the thing that had no handler at all before #744.
  printf 'device_del kbd0\n'
  sleep 20
  # PHASE 3: plug it back in, on the same root port.
  printf 'device_add usb-kbd,id=kbd0,bus=xhci.0,port=3\n'
  sleep 20
  # PHASE 4: and type on it. The whole point: no reboot, and it works.
  for _ in $(seq 1 60); do
    printf 'sendkey c\n'; sleep 0.2
  done
  # The mouse must survive it: a departure that took an unrelated device down with it
  # would be worse than not noticing at all.
  for _ in $(seq 1 40); do
    printf 'mouse_move 5 5\n'; sleep 0.2
  done
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
  -device usb-kbd,id=kbd0,bus=xhci.0,port=3 \
  -device usb-mouse,bus=xhci.0,port=2.4 \
  -serial "file:$S/serial.log" -monitor stdio -display none -vga std >$S/mon.log 2>$S/qemu.err || true

echo "=== claims"
grep -a "host-hid: USB .* CLAIMED\|no interrupt-IN block free" $S/serial.log | head -4
echo "=== transfer errors, if any"
grep -a "interrupt-IN transfer FAILED" $S/serial.log | head -4
echo "=== last counters"
grep -a "fw-1 DIAG: HID\[\|fw-1 DIAG: MOUSE\|fw-1 DIAG: host-kbd" $S/serial.log | tail -5

echo "=== the departure"
grep -a "port .* changed\|DEPARTED\|Disable Slot" $S/serial.log | head -8

fail=0
say() { echo "$1"; fail=1; }
grep -aq "host-hid: USB keyboard CLAIMED" $S/serial.log || say "FAIL: the keyboard was not claimed"
grep -aq "host-hid: USB mouse CLAIMED" $S/serial.log || say "FAIL: the mouse was not claimed"
r1=$(cat $S/r1.txt 2>/dev/null)
[ "${r1:-0}" -gt 0 ] 2>/dev/null || say "FAIL: the keyboard never reported before the unplug"

# #744's half still has to hold.
grep -aq "host-hid: keyboard .* DEPARTED" $S/serial.log ||
  say "FAIL: the departure was not noticed [#744]"

# #745's own bar.
grep -aq "host-usb: port .* ARRIVED" $S/serial.log ||
  say "FAIL: the re-plug was never enumerated [#745]"
# Two CLAIMED lines: the boot one and the re-plug one. One means the arrival enumerated
# but was not claimed, which is a keyboard that exists and does nothing.
nclaim=$(grep -a -c "host-hid: USB keyboard CLAIMED" $S/serial.log)
echo "keyboard CLAIMED lines: $nclaim (expect 2 -- boot, then the re-plug)"
[ "$nclaim" -ge 2 ] || say "FAIL: the arrived keyboard was not claimed [#745]"
# And it must actually report afterwards. A claim that enumerates but never delivers a
# report is exactly the failure #217's own FIRST-report line exists to distinguish.
nfirst=$(grep -a -c "host-hid: FIRST report received" $S/serial.log)
echo "FIRST-report lines: $nfirst (expect 2 -- one per claim)"
[ "$nfirst" -ge 2 ] || say "FAIL: the re-plugged keyboard never reported [#745]"

# The mouse is the control: it is behind the hub, untouched, and must still be reporting
# after the keyboard left. A teardown that took a bystander with it is a worse bug.
mouse_after=$(grep -a "fw-1 DIAG: MOUSE" $S/serial.log | tail -1 | sed -n 's/.*reports=\([0-9]*\).*/\1/p')
echo "mouse reports at the end: ${mouse_after:-none}"
[ "${mouse_after:-0}" -gt 0 ] 2>/dev/null ||
  say "FAIL: the mouse stopped reporting when the keyboard departed [#744]"

# And hype must still be alive and polling -- a departure must not wedge the input tick.
grep -aq "fw-1 DIAG: host-kbd" $S/serial.log ||
  say "FAIL: no host-kbd DIAG after the unplug -- the input tick stopped [#744]"

[ $fail -eq 0 ] && echo "ALL PASS: unplugged, re-plugged, enumerated, claimed and typing again -- no reboot"
exit $fail
