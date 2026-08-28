#!/bin/bash
# #737 / #736 validation: a USB HID behind a hub, which is the topology the 5950X has
# and no rig had. hype must
#   1. enumerate the hub and mark its slot as a hub (Hub=1, port count, TTT),
#   2. Address Device the HID behind it without a Parameter Error,
#   3. claim it, and
#   4. actually receive reports -- QEMU's monitor injects the keystrokes.
#
# Before the fix, step 4 was the one that silently failed on real hardware: 68818
# interrupt-IN polls, reports=0. A rig that stops at "CLAIMED" proves nothing.
#
# WHAT THIS RIG CANNOT DO -- measured, not assumed. It passes identically with the
# #736/#737 fix reverted (keyboard reports=62, mouse reports=38 on a build with the
# hub-slot Configure Endpoint disabled and Max ESIT Payload back at 0). Two reasons:
#   - QEMU's usb-hub attaches at FULL speed ("port 6 dev -- USB 1.10"), so a
#     full-speed child needs no Transaction Translator and the TT path -- the whole
#     subject of #737 -- is never entered;
#   - QEMU's xHCI model does not enforce the Hub bit or periodic-bandwidth
#     reservation, so a context the real controller rejects works here.
# So this is a NO-REGRESSION gate for the hub-descent + HID-report path, not proof of
# either fix. Only the 5950X (high-speed hub, full-speed keyboard) can prove those.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S=rig/i737
SECS="${1:-180}"
mkdir -p $S
rm -f $S/serial.log $S/esp.img $S/usb.img $S/mon.sock

killall -9 "$(basename "$QEMU")" 2>/dev/null || true
sleep 1
pidof qemu-system-x86_64 >/dev/null && { echo "FAIL: a qemu is still running"; exit 1; }

dd if=/dev/zero of=$S/usb.img bs=1M count=64 status=none
mkfs.vfat -F 32 -n HYPEUSB $S/usb.img >/dev/null

dd if=/dev/zero of=$S/esp.img bs=1M count=512 status=none
mkfs.vfat -F 32 -n HYPEESP $S/esp.img >/dev/null
mmd -i $S/esp.img ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/hype ::/hype/disks
mcopy -i $S/esp.img build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i $S/esp.img fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i $S/esp.img disk-images/alpine-virt-console.iso ::/iso/test.iso
mcopy -i $S/esp.img tools/737/hype.cfg ::/hype.cfg

cp /usr/share/edk2/ovmf/OVMF_VARS.fd $S/VARS.fd

# The input injection: wait for hype to reach its guest loop (the HID poll only runs
# there), then type. Runs alongside qemu and dies with it.
(
  for _ in $(seq 1 "$SECS"); do
    sleep 1
    grep -aq "fw-1 DIAG: HID" $S/serial.log 2>/dev/null && break
  done
  for _ in $(seq 1 50); do
    printf 'sendkey a\n'; sleep 0.2
    printf 'mouse_move 5 5\n'; sleep 0.2
  done
  # The DIAG counters are printed on a ~35s cadence, so quitting straight after the
  # keystrokes leaves only the pre-input line in the log and reads as reports=0.
  sleep 110
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
  -device usb-storage,bus=xhci.0,port=1,drive=stick,serial=HYPE737USB \
  -device usb-hub,id=hub1,bus=xhci.0,port=2,ports=4 \
  -device usb-kbd,bus=xhci.0,port=2.2 \
  -device usb-mouse,bus=xhci.0,port=2.4 \
  -serial "file:$S/serial.log" -monitor stdio -display none -vga std >$S/mon.log 2>$S/qemu.err || true

echo "=== hub descent"
grep -a "is a USB hub\|hub slot .* downstream port\|Configure Endpoint (Hub=1" $S/serial.log | head -8
echo "=== devices behind the hub"
grep -a "hub slot .* port .* dev \|behind-hub device" $S/serial.log | head -8
echo "=== claims + reports"
grep -a "host-hid: USB \|Address Device slot .* completion code" $S/serial.log | head -8
grep -a "fw-1 DIAG: HID\|fw-1 DIAG: MOUSE" $S/serial.log | tail -2

fail=0
say() { echo "$1"; fail=1; }
grep -aq "hub slot .* has 4 downstream port(s)" $S/serial.log || say "FAIL: the hub was never walked"
grep -aq "Configure Endpoint (Hub=1" $S/serial.log && say "FAIL: marking the hub slot as a hub failed [#737]"
grep -aq "Address Device slot .* completion code 17" $S/serial.log && say "FAIL: Parameter Error addressing a device behind the hub [#737]"
grep -aq "host-hid: USB keyboard CLAIMED" $S/serial.log || say "FAIL: the keyboard behind the hub was not claimed"
# reports= is the whole point: a claimed HID that never reports is exactly #736.
kb=$(grep -a "fw-1 DIAG: HID" $S/serial.log | tail -1 | sed -n 's/.*reports=\([0-9]*\).*/\1/p')
ms=$(grep -a "fw-1 DIAG: MOUSE" $S/serial.log | tail -1 | sed -n 's/.*reports=\([0-9]*\).*/\1/p')
echo "keyboard reports=${kb:-none} mouse reports=${ms:-none}"
[ "${kb:-0}" -gt 0 ] 2>/dev/null || say "FAIL: keyboard claimed but reports=0 [#736]"
[ "${ms:-0}" -gt 0 ] 2>/dev/null || say "FAIL: mouse claimed but reports=0 [#736]"
[ $fail -eq 0 ] && echo "ALL PASS: HID behind a hub enumerated, addressed, claimed, and reporting"
exit $fail
