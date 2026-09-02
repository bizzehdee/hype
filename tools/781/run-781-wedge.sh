#!/bin/bash
#
# #782/#783/#785 (decision 75): wedge an xHCI command ring ON DEMAND, and watch hype recover.
#
# The real wedge appeared in three boots out of eleven and every observation cost a cold boot.
# This rig builds hype with -DHYPE_781_WEDGE_MS so the first command on the chosen controller
# after 60 s never completes and the Command Abort never takes -- boot 35's exact shape -- then
# provokes that command by hot-plugging a keyboard onto the controller.
#
# Two controllers, the 5950X's arrangement (tools/767): xhci0 carries hype's log/boot stick,
# xhci1 carries every human-input device behind a hub.
#
#   run-781-wedge.sh 2   wedge ctrl[2] (input): hype must RESET it, re-enumerate the hub and
#                        keyboard, re-claim the keyboard, and the log on ctrl[1] must record
#                        the whole thing.
#   run-781-wedge.sh 1   wedge ctrl[1] (log + boot medium): hype must REFUSE, say why, stay
#                        dead on that controller, and the log -- written through bulk transfers
#                        on the wedged controller -- must still carry the refusal.
#
# `make clean` runs first: make ignores EXTRA_CFLAGS changes, and a stale binary is
# indistinguishable from a fix that did not work. The run is gated on the binary's own echo of
# the injection ("WEDGE INJECTION ARMED -- ctrl[N]"), not on this script's belief about it.
# The tree is left on a DEFAULT build afterwards.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
W="${1:-2}"
SECS="${2:-420}"
WEDGE_MS=60000
S=rig/i781
mkdir -p $S
rm -f $S/serial.log $S/esp.img $S/usb.img $S/mon.log $S/qemu.err $S/HYPE.LOG

killall -9 "$(basename "$QEMU")" 2>/dev/null || true
sleep 1
pidof qemu-system-x86_64 >/dev/null && { echo "FAIL: a qemu is still running"; exit 1; }

echo "=== building the injection variant (ctrl[$W], ${WEDGE_MS} ms) ==="
make clean >/dev/null
make all EXTRA_CFLAGS="-DHYPE_781_WEDGE_MS=${WEDGE_MS}u -DHYPE_781_WEDGE_CTRL=${W}u" >/dev/null
SHA=$(git rev-parse --short HEAD)

# The stick is hype's OWN boot volume by content (#638: loader + firmware), so the log sink
# opens on it and ctrl[1] becomes the protected controller.
dd if=/dev/zero of=$S/usb.img bs=1M count=64 status=none
mkfs.vfat -F 32 -n HYPEUSB $S/usb.img >/dev/null
mmd -i $S/usb.img ::/EFI ::/EFI/BOOT ::/EFI/hype
mcopy -i $S/usb.img build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i $S/usb.img fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i $S/usb.img tools/734/hype.cfg ::/hype.cfg

dd if=/dev/zero of=$S/esp.img bs=1M count=512 status=none
mkfs.vfat -F 32 -n HYPEESP $S/esp.img >/dev/null
mmd -i $S/esp.img ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/hype ::/hype/disks
mcopy -i $S/esp.img build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i $S/esp.img fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i $S/esp.img disk-images/alpine-virt-console.iso ::/iso/test.iso
mcopy -i $S/esp.img tools/734/hype.cfg ::/hype.cfg
cp /usr/share/edk2/ovmf/OVMF_VARS.fd $S/VARS.fd

(
  # Both controllers are up once the inventory prints. The wedge clock started at each
  # controller's bring-up, so 65 s after the inventory the next command on ctrl[W] wedges.
  for _ in $(seq 1 150); do
    sleep 1
    grep -aq "INVENTORY --" $S/serial.log 2>/dev/null && break
  done
  sleep 65
  # The command: a keyboard arriving on a root port of the wedged controller needs Enable Slot.
  printf 'device_add usb-kbd,id=kbdw,bus=xhci%s.0,port=3\n' "$((W-1))"
  # Timeout (1 s), abort (5 s), then the next tick decides. Wait for that decision.
  for _ in $(seq 1 90); do
    sleep 1
    grep -aqE "XHCIRESET ctrl\[$W\]: (reset #1 (done|FAILED)|REFUSED)" $S/serial.log 2>/dev/null && break
  done
  sleep 3
  # Type on the ORIGINAL keyboard: after a reset of ctrl[2] it must report again.
  for _ in $(seq 1 40); do printf 'sendkey a\n'; sleep 0.15; done
  sleep 15
  printf 'quit\n'
) | timeout "$SECS" "$QEMU" \
  -machine q35 -m 4096 -nodefaults \
  -accel kvm -cpu host,topoext=on -smp cpus=4,sockets=1,cores=2,threads=2 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=$S/VARS.fd \
  -drive format=raw,file=$S/esp.img,if=none,id=esp \
  -device ide-hd,drive=esp,bus=ide.0,bootindex=0 \
  -device qemu-xhci,id=xhci0 \
  -device qemu-xhci,id=xhci1 \
  -drive format=raw,file=$S/usb.img,if=none,id=stick \
  -device usb-storage,bus=xhci0.0,port=1,drive=stick,serial=HYPE781USB \
  -device usb-hub,id=hubA,bus=xhci1.0,port=1,ports=4 \
  -device usb-kbd,id=kbd0,bus=xhci1.0,port=1.2 \
  -device usb-mouse,id=mou0,bus=xhci1.0,port=1.4 \
  -serial "file:$S/serial.log" -monitor stdio -display none -vga std >$S/mon.log 2>$S/qemu.err || true

echo "=== leaving the tree on a default build ==="
make clean >/dev/null
make all >/dev/null

L=$S/serial.log
echo "=== what hype said ==="
grep -a "hype: build\|XHCIOWN\|INJECTED WEDGE\|#266 command TIMEOUT\|stopped answering\|still RUNNING\|XHCIRESET\|Enable Slot FAILED\|USB keyboard CLAIMED\|FIRST report" $L | cut -c1-200

fail() { echo "FAIL: $*"; exit 1; }
grep -aq "hype: build" $L || { echo "NOBOOT: hype never ran (#371)"; exit 2; }
grep -aq "hype: build $SHA" $L || fail "banner sha is not $SHA -- a stale binary ran"
grep -aq "WEDGE INJECTION ARMED -- ctrl\[$W\]'s first command after $WEDGE_MS ms" $L ||
  fail "the binary did not echo the injection for ctrl[$W] -- EXTRA_CFLAGS did not take"
grep -aq "XHCIOWN: log sink on ctrl\[1\], boot medium on ctrl\[1\]" $L ||
  fail "the ownership line did not name ctrl[1] for both roles"
grep -aq "host-xhci: controller\[2\]" $L || { echo "INVALID: only one controller"; exit 2; }
grep -aq "INJECTED WEDGE on ctrl$W" $L || fail "the wedge never fired -- was a command issued on ctrl[$W]?"
grep -aq "#266 command TIMEOUT" $L || fail "no #266 TIMEOUT line (boot 35's first line)"
grep -aq "ctrl$W command ring stopped answering -- aborting it" $L || fail "no abort line"
grep -aq "command ring still RUNNING 5000 ms after Command Abort" $L || fail "the abort took -- the injection is easier than the real fault"
[ "$(grep -ac "INJECTED WEDGE" $L)" = "1" ] || fail "the injection fired more than once"

if [ "$W" = "2" ]; then
  grep -aq "XHCIRESET ctrl\[2\]: reset #1 begins" $L || fail "no reset was attempted on ctrl[2]"
  DONE=$(grep -a "XHCIRESET ctrl\[2\]: reset #1 done" $L | head -1)
  [ -n "$DONE" ] || fail "the reset did not complete"
  echo "$DONE" | grep -q "keyboards=[1-9]" || fail "no keyboard came back after the reset: $DONE"
  echo "$DONE" | grep -q "hubs=[1-9]" || fail "the hub was not among what was released: $DONE"
  # Re-claimed, and reporting again: the sendkeys after the reset must produce a first report.
  # The re-claim happens INSIDE the reset, so it is logged between "begins" and "done".
  AFTER=$(grep -a -n "XHCIRESET ctrl\[2\]: reset #1 begins" $L | head -1 | cut -d: -f1)
  tail -n +"$AFTER" $L | grep -aq "USB keyboard CLAIMED" || fail "no keyboard CLAIMED after the reset"
  tail -n +"$AFTER" $L | grep -aq "FIRST report received" || fail "the re-claimed keyboard never reported"
  tail -n +"$AFTER" $L | grep -aq "INJECTED WEDGE" && fail "the wedge re-fired after the reset"
  # The log on ctrl[1] survived the reset of ctrl[2] and recorded it.
  mcopy -n -i $S/usb.img ::/HYPE.LOG $S/HYPE.LOG 2>/dev/null || fail "no HYPE.LOG on the stick"
  grep -aq "XHCIRESET ctrl\[2\]: reset #1 done" $S/HYPE.LOG || fail "the on-stick log does not carry the reset"
  echo "ALL PASS: ctrl[2] wedged on demand, was reset, its hub and keyboard came back and reported; the log on ctrl[1] recorded it [#782 #784 #785]"
else
  grep -aq "XHCIRESET ctrl\[1\]: REFUSED -- it carries the log sink and the boot medium" $L ||
    fail "the log controller was not refused with the right reason"
  grep -aq "XHCIRESET ctrl\[1\]: reset #1 begins" $L && fail "hype reset the controller carrying its log"
  mcopy -n -i $S/usb.img ::/HYPE.LOG $S/HYPE.LOG 2>/dev/null || fail "no HYPE.LOG on the stick"
  grep -aq "XHCIRESET ctrl\[1\]: REFUSED" $S/HYPE.LOG || fail "the refusal did not reach the on-stick log -- the evidence was lost"
  echo "ALL PASS: ctrl[1] wedged on demand, hype refused to reset the controller carrying its log and boot medium, and the on-stick log carries the refusal [#782 #783]"
fi
