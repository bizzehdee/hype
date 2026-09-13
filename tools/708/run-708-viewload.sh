#!/bin/bash
#
# #708: does switching INTO a VM view under load kill the host?
#
# The operator, after the boot that raised the wedge alert: "i didnt see the message, i
# switched away to a vm for a second, and thats when everything locked up". Three boots agree:
#
#   boot g  VIEWSWITCH -> view=3 logged at 436,887, complete 437,281, log ends 437,882
#   boot i  the switch into a VM produced NO VIEWSWITCH line at all; the log ends there
#   16af29c "host then froze right after a view switch to vm0"
#
# It also explains why the alert was never read: the alert is only drawn on the DASHBOARD
# branch, so switching to a VM hides it by construction.
#
# This is tools/708/run-708-fsload.sh's load -- three guests, images on exFAT, ext4 and NTFS,
# the 64 MiB oflag=direct writes -- with the view cycled through every VM and back throughout,
# including vm3, which is configured but never started and is what boot g's last switch
# selected. tools/708/run-708-viewswitch.sh switched views but kept its images on AHCI, so it
# never combined the two.
#
# FREEZE = FBSPEED stops for 30 s. PASS = all three scripts PASS with the cycling running.
#
#   EFI=<path> tools/708/run-708-viewload.sh     default EFI: build/hype.efi
set -u
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"

EFI="${EFI:-build/hype.efi}"
S="${SCRATCH:-rig/708-viewload}"
CFG="${CFG:-tools/hw-val-2026-08-25/hype2h.cfg}"
DWELL="${DWELL:-3}"          # seconds to sit in each view
ROUNDS="${ROUNDS:-20}"
. tools/708/make-fsload-image.sh

LOG="$S"/serial.log
fbcount() { grep -a -c "FBSPEED: t=" "$LOG" 2>/dev/null || echo 0; }
{
  for _ in $(seq 1 900); do
    sleep 1
    grep -aq "ttyS0| READBACK-MATCH" "$LOG" 2>/dev/null && break
  done
  echo "view cycling starts at $(date +%T)" >&2
  # Keep cycling for the WHOLE run, scripts passing or not: the freeze the operator hits
  # happens while they are switching, and the first cut stopped the moment the three scripts
  # finished -- 41 seconds of cycling, of which hype logged two switches. Chords are also
  # dropped under load (the i8042 holds one byte), which is the operator's "some switches are
  # skipped", so send each one twice and count what hype actually took rather than what was
  # sent.
  r=0
  while [ "$r" -lt "$ROUNDS" ]; do
    for v in 1 2 3 4 d; do
      printf 'sendkey ctrl_r-alt_r-%s\n' "$v"
      sleep 0.4
      printf 'sendkey ctrl_r-alt_r-%s\n' "$v"
      sleep "$DWELL"
    done
    r=$((r + 1))
  done
  echo "view cycling done at $(date +%T)" >&2
  last=$(fbcount); still=0
  for _ in $(seq 1 300); do
    sleep 1
    now=$(fbcount)
    if [ "$now" = "$last" ]; then still=$((still + 1)); else still=0; last=$now; fi
    [ "$(grep -a -c -E 'SCRIPT vm[0-2]: PASS' "$LOG")" -ge 3 ] && break
    if [ "$still" -ge 30 ]; then
      echo "FBSPEED stalled 30 s at $(date +%T)" >&2
      for i in 1 2 3; do printf 'info registers -a\n'; sleep 3; done
      printf 'screendump %s/freeze.ppm\n' "$S"; sleep 3
      break
    fi
  done
  sleep 5
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
  -serial "file:$LOG" -monitor stdio -display none -vga std >"$S"/mon.log 2>"$S"/qemu.err || true

echo "=== build ==="
grep -a -m1 "^hype: build" "$LOG"
echo "=== view switches ==="
grep -a -c "VIEWSWITCH: action" "$LOG" | sed 's/^/  switches requested: /'
grep -a "VIEWSWITCH" "$LOG" | tail -4 | cut -c1-110
echo "=== USB lock ==="
grep -a -o -E "USBLOCK: .*held max=[0-9]+us by apic=-?[0-9]+ vm-?[0-9]+ \| now=[0-9]+us apic=-?[0-9]+" "$LOG" | tail -1
grep -a -c "USB WEDGED" "$LOG" | sed 's/^/  USB WEDGED log lines: /'
echo "=== scripts ==="
grep -a -E "SCRIPT vm[0-9]: (PASS|FAIL)" "$LOG" | cut -c1-90
grep -a -o -E "FBSPEED: t=[0-9]+ms" "$LOG" | tail -1

SW=$(grep -a -c "VIEWSWITCH: action" "$LOG")
if [ "${SW:-0}" -lt 10 ]; then
  echo "INVALID: only $SW view switch(es) were taken -- the chords never reached hype, so this"
  echo "         run tested the load and not the thing it exists to test"
  exit 2
fi
if [ -f "$S"/freeze.ppm ]; then
  echo "FREEZE: FBSPEED stopped while cycling views under load [#708]"
  grep -a -o -E "CPU#[0-9]+|RIP=[0-9a-f]{16}" "$S"/mon.log | paste - - | sort | uniq -c
  tail -25 "$LOG" | cut -c1-190
  exit 1
fi
if [ "$(grep -a -c -E 'SCRIPT vm[0-2]: PASS' "$LOG")" -ge 3 ]; then
  echo "PASS: $SW view switches under the full load, all three scripts finished [#708]"; exit 0
fi
echo "INCONCLUSIVE: no freeze but not every script passed"; tail -20 "$LOG" | cut -c1-190; exit 3
