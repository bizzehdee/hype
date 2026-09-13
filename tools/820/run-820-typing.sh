#!/bin/bash
#
# #820: how long does it take to type at hype's dashboard while three guests hammer the USB
# drive their disks live on?
#
# The operator's report is "30-40 seconds to type `host off`" -- nine characters -- on runs
# whose BSPSTARVE and BSPCOST numbers look healthy. Nothing measured that, so every explanation
# offered for it was a guess. boot/main.c now carries #820's KEYLAT counters, which split the
# path into the only two places it can be lost:
#
#   poll gap   how often the BSP reaches the host keyboard at all
#   char gap   time between two characters hype actually accepted
#   echo       accepted character -> the push that put it on the glass
#
# This rig drives the same three-guest load as tools/708/run-708-fsload.sh -- the images on
# exFAT, ext4 and NTFS, the 64 MiB oflag=direct writes -- and types at the dashboard through
# the whole of it, so the numbers come out of a machine under the load being complained about
# rather than an idle one.
#
# Deliberately types letters and backspaces only, never Enter: no command runs, so the rig can
# hammer the command line for a minute without changing the state of the run.
#
# The keyboard is a USB HID on the SAME xHCI controller as the boot stick, the guest images and
# hype's log -- as it is on the i5, where the operator types on a USB Keychron. That is the whole
# point: hype's host-input poll and the guests' disk writes then contend for one USB transfer
# lock. The first cut of this rig let QEMU's `sendkey` go to the i8042 instead, which bypasses
# the USB path entirely, and measured a machine whose keyboard had no contention at all
# (echo mean 3 ms, max 381 ms) -- a number that says nothing about the reported fault.
#
#   EFI=<path> tools/820/run-820-typing.sh    default EFI: build/hype.efi
set -u
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"

EFI="${EFI:-build/hype.efi}"
S="${SCRATCH:-rig/820-typing}"
CFG="${CFG:-tools/hw-val-2026-08-25/hype2h.cfg}"
KEYS="${KEYS:-240}"          # characters to type
SPACING_MS="${SPACING_MS:-250}"
. tools/708/make-fsload-image.sh

LOG="$S"/serial.log
# Ten letters then ten backspaces: the command line fills and empties and never runs anything.
TYPE_SEQ=(a b c d e f g h i j backspace backspace backspace backspace backspace \
          backspace backspace backspace backspace backspace)
{
  # Wait until a guest has read its signature back -- from here the 64 MiB writes are starting
  # and the drive is as busy as it gets.
  for _ in $(seq 1 900); do
    sleep 1
    grep -aq "ttyS0| READBACK-MATCH" "$LOG" 2>/dev/null && break
  done
  echo "typing starts at $(date +%T)" >&2
  i=0
  while [ "$i" -lt "$KEYS" ]; do
    printf 'sendkey %s\n' "${TYPE_SEQ[$((i % ${#TYPE_SEQ[@]}))]}"
    i=$((i + 1))
    sleep "$(awk -v m="$SPACING_MS" 'BEGIN{printf "%.3f", m/1000}')"
  done
  echo "typing done at $(date +%T)" >&2
  # Let the 10-second KEYLAT beat fire at least twice more with the typing counted.
  sleep 25
  printf 'quit\n'
} | timeout 2700 "$QEMU" -machine q35 -m 12288 -nodefaults \
  -accel kvm -cpu host -smp 16,sockets=1,cores=8,threads=2 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file="$S"/VARS.fd \
  -device qemu-xhci,id=xhci \
  -drive format=raw,file="$S"/usb.img,if=none,id=stick \
  -device usb-storage,bus=xhci.0,drive=stick,serial=DB9876543214E,bootindex=0 \
  -drive format=raw,file="$S"/target.img,if=none,id=tgt \
  -device usb-storage,bus=xhci.0,drive=tgt,serial=03025220071724203145 \
  -device usb-kbd,bus=xhci.0 \
  -serial "file:$LOG" -monitor stdio -display none -vga std >"$S"/mon.log 2>"$S"/qemu.err || true

echo "=== build ==="
grep -a -m1 "^hype: build" "$LOG"
echo "=== typing latency (buckets: <1ms <10ms <50ms <200ms <1s >=1s) ==="
grep -a "KEYLAT" "$LOG" | tail -2 | cut -c1-260
echo "=== BSP stalls and phase cost ==="
grep -a "BSPSTARVE" "$LOG" | tail -1 | cut -c1-220
grep -a -E "BSPCOST (input|kbddiag|render|band|gopflush|dash|flush|vars) " "$LOG" | tail -8 | cut -c1-95
grep -a -o -E "BSPCOST input tick fired [0-9]+ times in [0-9]+s = [0-9]+ Hz \(want [0-9]+\)" "$LOG" | tail -1
echo "=== render and load ==="
grep -a "RENDERHIST" "$LOG" | tail -1 | cut -c1-140
grep -a -E "SCRIPT vm[0-9]: (PASS|FAIL)" "$LOG" | cut -c1-90

echo "=== USB HID endpoint ==="
grep -a "HIDTICK" "$LOG" | tail -1 | cut -c1-170

# The bar is not "some key arrived": it is that the characters TYPED are the characters hype
# accepted. The first USB-keyboard run took 1 of 240, with the endpoint armed 6 times in the
# whole run -- a pass gate of "chars != 0" would have called that a success.
TYPED=$(grep -a -o "chars=[0-9]*" "$LOG" | tail -1 | cut -d= -f2)
TYPED=${TYPED:-0}
WANT=$(( KEYS * 9 / 10 ))
if [ "$TYPED" -lt "$WANT" ]; then
  echo "FAIL: sent $KEYS key(s), hype accepted $TYPED -- $((KEYS - TYPED)) lost before the"
  echo "      command line. Read HIDTICK's arms/reports above: an interrupt-IN endpoint that"
  echo "      is not armed has nowhere to put a report, and a HID boot report is a state"
  echo "      snapshot, so a press+release inside that window leaves no trace at all [#773]"
  exit 1
fi
echo "PASS: sent $KEYS key(s), hype accepted $TYPED at the dashboard under load [#820]"
