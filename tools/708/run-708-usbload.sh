#!/bin/bash
#
# #708 follow-up: does the boot-1 load freeze the host when the guest disks live on USB?
#
# Two i5 boots (logs/bootI5-boot1-2026-09-12c, -2026-09-13) locked the whole host at the same
# moment: run2n had read back its signature and issued `dd ... of=/dev/vda bs=1M count=64
# oflag=direct`, and the log stops within a second. The second boot had no view switch, so the
# switch is not the trigger. On the i5 every guest disk image, both ISOs and hype's own log sit on
# one USB-SATA drive, so that write goes through the one USB transfer lock the log flush and the
# other guests' ISO reads also take. tools/708/run-708-viewswitch.sh kept its images on AHCI and
# passed -- it never took this path.
#
# This rig boots from one QEMU usb-storage device that also holds the ISO, all three images and
# hype's log. hype's serial is a file, so a freeze
# cannot lose the tail. Once run2n reads back, the watcher waits for the BSP's FBSPEED lines; if
# they stop for 30 s it records every CPU's registers and a screenshot from the monitor, then quits.
#
# FREEZE = FBSPEED stalls after run2n's READBACK-MATCH. PASS = all three scripts PASS.
#
#   EFI=<path> tools/708/run-708-usbload.sh      default EFI: rig/stage-current/hype-default.efi
set -u
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"

EFI="${EFI:-rig/stage-current/hype-default.efi}"
S="${SCRATCH:-rig/708-usb}"
ISO=disk-images/hwval-data-2026-09-09/iso/test.iso
HERE=tools/hw-val-2026-08-25
[ -f "$EFI" ] || { echo "no $EFI"; exit 2; }
[ -f "$ISO" ] || { echo "no ISO at $ISO"; exit 2; }

rm -rf "$S"; mkdir -p "$S"
# One USB drive, booted from, like the i5: GPT with a FAT32 partition holding hype, firmware, config,
# input scripts, the ISO and the three images. hype writes its log only to the volume it booted
# from (#638) and resolves media only in GPT partitions 1-4, so a partitionless stick beside an
# AHCI ESP (this rig's first cut) put neither the log nor the images on USB.
dd if=/dev/zero of="$S"/usb.img bs=1M count=3800 conv=fsync status=none
printf '2048,,U\n' | sfdisk --label gpt -q "$S"/usb.img
mformat -i "$S"/usb.img@@1M -F -v HYPEBOOT ::
mmd -i "$S"/usb.img@@1M ::/EFI ::/EFI/BOOT ::/EFI/hype ::/input ::/iso ::/hype ::/hype/disks
mcopy -i "$S"/usb.img@@1M "$EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$S"/usb.img@@1M fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
for v in 0 1 2; do mcopy -i "$S"/usb.img@@1M "$HERE/input-2h/vm$v.txt" "::/input/vm$v.txt"; done
cat > "$S"/hype.cfg <<'CFG'
[hype]
config_version = 1
log_level = debug
autostart = run2c, run2e, run2n

[vm.run2c]
vcpus = 2
mem_mb = 1024
boot = installer
install_media = \iso\test.iso
firmware = uefi
os_hint = linux
target_disk = file:\hype\disks\run2c.img
target_disk_size_gb = 1

[vm.run2e]
vcpus = 1
mem_mb = 512
boot = installer
install_media = \iso\test.iso
firmware = uefi
os_hint = linux
target_disk = file:\hype\disks\run2e.img
target_disk_size_gb = 1

[vm.run2n]
vcpus = 1
mem_mb = 512
boot = installer
install_media = \iso\test.iso
firmware = uefi
os_hint = linux
target_disk = file:\hype\disks\run2n.img
target_disk_size_gb = 1
CFG
mcopy -i "$S"/usb.img@@1M "$S"/hype.cfg ::/hype.cfg
mcopy -i "$S"/usb.img@@1M "$ISO" ::/iso/test.iso
dd if=/dev/zero of="$S"/zero1g.img bs=1M count=1024 status=none
for d in run2c run2e run2n; do mcopy -i "$S"/usb.img@@1M "$S"/zero1g.img "::/hype/disks/$d.img"; done
rm -f "$S"/zero1g.img
cp /usr/share/edk2/ovmf/OVMF_VARS.fd "$S"/VARS.fd

LOG="$S"/serial.log
fbcount() { grep -a -c "FBSPEED: t=" "$LOG" 2>/dev/null || echo 0; }
{
  for _ in $(seq 1 900); do
    sleep 1
    grep -aq "vm2 ttyS0| READBACK-MATCH" "$LOG" 2>/dev/null && break
  done
  echo "readback at $(date +%T)" >&2
  last=$(fbcount); still=0
  for _ in $(seq 1 600); do
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
} | timeout 1800 "$QEMU" -machine q35 -m 12288 -nodefaults \
  -accel kvm -cpu host -smp 16,sockets=1,cores=8,threads=2 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file="$S"/VARS.fd \
  -device qemu-xhci,id=xhci \
  -drive format=raw,file="$S"/usb.img,if=none,id=stick \
  -device usb-storage,bus=xhci.0,drive=stick,serial=HYPEUSB708,bootindex=0 \
  -serial "file:$LOG" -monitor stdio -display none -vga std >"$S"/mon.log 2>"$S"/qemu.err || true

echo "=== build / vendor ==="
grep -a -m2 -E "^hype: build|vmm: (SVM|VMX) detected" "$LOG"
echo "PANIC=$(grep -a -c PANIC "$LOG") APSTACK_OVERFLOW=$(grep -a -c 'APSTACK OVERFLOW' "$LOG")"
echo "=== disks and log sink ==="
grep -a -E "m5-8: (FILE-backed|target_disk)|host-fat: vm[0-9] resolved|usb-log: |XHCIOWN" "$LOG" | cut -c1-150 | head -8
echo "=== scripts ==="
grep -a -E "SCRIPT vm[0-9]: (PASS|FAIL)" "$LOG" | cut -c1-120
grep -a -E "vm[0-9] ttyS0\| (READBACK-MATCH|BULK-[0-9]+|EXT4-WRITE-DONE|NTFS-WRITE-DONE|BOOT-OK-)" "$LOG" | cut -c1-100
grep -a -o -E "FBSPEED: t=[0-9]+ms" "$LOG" | tail -1
if ! grep -aq "vm2 ttyS0| READBACK-MATCH" "$LOG"; then
  echo "INVALID: run2n never read back its signature -- the i5's trigger was never reached"; exit 2
fi
if [ -f "$S"/freeze.ppm ]; then
  echo "FREEZE: the BSP stopped logging after run2n's READBACK-MATCH [#708]"
  grep -a -o -E "CPU#[0-9]+|RIP=[0-9a-f]{16}" "$S"/mon.log | paste - - | sort | uniq -c
  tail -30 "$LOG" | cut -c1-200
  exit 1
fi
if [ "$(grep -a -c -E 'SCRIPT vm[0-2]: PASS' "$LOG")" -ge 3 ]; then
  echo "PASS: all three scripts finished with the guest disks on USB [#708]"; exit 0
fi
echo "INCONCLUSIVE: no freeze detected but not every script passed"; tail -20 "$LOG" | cut -c1-200; exit 3
