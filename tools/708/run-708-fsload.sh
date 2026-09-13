#!/bin/bash
#
# #708 follow-up: reproduce the i5 host freeze with the guest images on the SAME host
# filesystems the hw-val drive uses.
#
# Three i5 boots froze the whole host within a second of a guest issuing
# `dd ... of=/dev/vda bs=1M count=64 oflag=direct` to its image:
#   bootI5 2026-09-12c and 2026-09-13a  run2n, image on the drive's NTFS partition
#   bootI5 2026-09-13b                  run2e, image on the drive's ext4 partition
#                                       (run2n's own 64 MiB write had already PASSED)
# So it is not one filesystem, not the first write, and not the view switch.
#
# tools/708/run-708-usbload.sh put every file on ONE FAT32 partition and PASSED -- it never
# takes the ext/NTFS in-place writer at all. This rig mirrors the drive instead:
#   p1 FAT32  HYPEBOOT   hype, firmware, hype.cfg, input scripts, and hype's own log
#   p2 exFAT  HYPEDATA   \hype\disks\run2c-scratch.img
#   p3 ext4   HYPEEXT4   \iso\test.iso      + \hype\disks\ext4-scratch.img   (#688)
#   p4 NTFS   HYPENTFS   \iso\ntfs-test.iso + \hype\disks\ntfs-scratch.img   (#689)
# on one QEMU usb-storage device whose serial is the drive's own, so the staged hype.cfg
# is used verbatim. hype's serial is a file, so a freeze cannot lose the tail.
#
# FREEZE = FBSPEED stops for 30 s after the first READBACK-MATCH. PASS = all three scripts PASS.
#
#   EFI=<path> tools/708/run-708-fsload.sh     default EFI: rig/stage-current/hype-default.efiset -u
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"

EFI="${EFI:-rig/stage-current/hype-default.efi}"
S="${SCRATCH:-rig/708-fsload}"
CFG="${CFG:-tools/hw-val-2026-08-25/hype2h.cfg}"
. tools/708/make-fsload-image.sh

LOG="$S"/serial.log
fbcount() { grep -a -c "FBSPEED: t=" "$LOG" 2>/dev/null || echo 0; }
{
  for _ in $(seq 1 900); do
    sleep 1
    grep -aq "ttyS0| READBACK-MATCH" "$LOG" 2>/dev/null && break
  done
  echo "first readback at $(date +%T)" >&2
  last=$(fbcount); still=0
  for _ in $(seq 1 900); do
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

echo "=== build / vendor ==="
grep -a -m2 -E "^hype: build|vmm: (SVM|VMX) detected" "$LOG"
echo "PANIC=$(grep -a -c PANIC "$LOG") APSTACK_OVERFLOW=$(grep -a -c 'APSTACK OVERFLOW' "$LOG")"
echo "=== volumes, disks and log sink ==="
grep -a -E "m5-8: FILE-backed|host-fat: vm[0-9] resolved|usb-log: \\\\HYPE.LOG|XHCIOWN" "$LOG" | cut -c1-160 | head -10
echo "=== stack high-water ==="
grep -a "APSTACK:" "$LOG" | tail -2 | cut -c1-200
echo "=== scripts ==="
grep -a -E "SCRIPT vm[0-9]: (PASS|FAIL)" "$LOG" | cut -c1-120
grep -a -E "vm[0-9] ttyS0\| (READBACK-MATCH|BULK-[0-9]+|EXT4-WRITE-DONE|NTFS-WRITE-DONE|BOOT-OK-)" "$LOG" | cut -c1-100
grep -a -o -E "FBSPEED: t=[0-9]+ms" "$LOG" | tail -1

if ! grep -aq "m5-8: FILE-backed guest disk .*on ext" "$LOG" || \
   ! grep -aq "m5-8: FILE-backed guest disk .*on NTFS" "$LOG"; then
  echo "INVALID: the images did not resolve on ext AND NTFS -- the i5's path was never taken"; exit 2
fi
if ! grep -aq "ttyS0| READBACK-MATCH" "$LOG"; then
  echo "INVALID: no guest read back its signature -- the i5's trigger was never reached"; exit 2
fi
if [ -f "$S"/freeze.ppm ]; then
  echo "FREEZE: the BSP stopped logging after a guest's READBACK-MATCH [#708]"
  grep -a -o -E "CPU#[0-9]+|RIP=[0-9a-f]{16}" "$S"/mon.log | paste - - | sort | uniq -c
  tail -30 "$LOG" | cut -c1-200
  exit 1
fi
if [ "$(grep -a -c -E 'SCRIPT vm[0-2]: PASS' "$LOG")" -ge 3 ]; then
  echo "PASS: all three scripts finished with the images on exFAT, ext4 and NTFS [#708]"; exit 0
fi
echo "INCONCLUSIVE: no freeze detected but not every script passed"; tail -20 "$LOG" | cut -c1-200; exit 3
