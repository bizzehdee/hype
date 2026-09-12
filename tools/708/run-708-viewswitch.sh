#!/bin/bash
#
# #708 follow-up: does switching the console to a guest view freeze the BSP under the boot-1 load?
#
# The first i5 boot with the 8259-injection fix (logs/bootI5-boot1-2026-09-13) locked the whole
# host about 22 s in. The operator had just switched to vm0 (`VIEWSWITCH ... view=0 complete in
# 28ms over 1 render passes`); the screen showed a blank console with a cursor, the chord would not
# switch away, and the USB-SATA activity LED stayed dark for ~30 s until power-off. The BSP logged
# nothing after the switch -- only guest-core EOI probe lines follow -- and the last 43 KB of the
# log never reached the stick, so the log cannot say where the BSP stopped.
#
# This reproduces the load on this host, where hype's log goes to a serial file that a freeze
# cannot truncate: the hype2h layout (run2c 2 vCPUs, run2e and run2n 1 each, the same input
# scripts, file-backed target disks), then the same chord into vm0 once run2n has read back its
# signature and started its bulk write, then back to the dashboard, twice.
#
# FREEZE = the BSP's periodic lines (FBSPEED) stop after the first switch, or the switch back to the
# dashboard never logs. Not a freeze = both switches log and FBSPEED keeps advancing to the end.
#
#   tools/708/run-708-viewswitch.sh [build]      build: default (SVM here) | avic; default default
set -u
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"

VARIANT="${1:-default}"
EFI="rig/stage-current/hype-$VARIANT.efi"
S="${SCRATCH:-rig/708-view}"
ISO=disk-images/hwval-data-2026-09-09/iso/test.iso
HERE=tools/hw-val-2026-08-25
[ -f "$EFI" ] || { echo "no $EFI -- run stage.sh first"; exit 2; }
[ -f "$ISO" ] || { echo "no ISO at $ISO"; exit 2; }
strings -a "$EFI" | grep -q "injected from the 8259" || { echo "$EFI predates the #708 fix"; exit 2; }

rm -rf "$S"; mkdir -p "$S"
dd if=/dev/zero of="$S"/esp.img bs=1M count=3700 conv=fsync status=none
printf '2048,,U\n' | sfdisk --label gpt -q "$S"/esp.img
mformat -i "$S"/esp.img@@1M -F ::
mmd -i "$S"/esp.img@@1M ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/hype ::/hype/disks ::/input
mcopy -i "$S"/esp.img@@1M "$EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$S"/esp.img@@1M fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i "$S"/esp.img@@1M "$ISO" ::/iso/test.iso
# hype creates no images (#738) and only writes in place: real zeros, full size.
dd if=/dev/zero of="$S"/zero1g.img bs=1M count=1024 status=none
for d in run2c run2e run2n; do mcopy -i "$S"/esp.img@@1M "$S"/zero1g.img "::/hype/disks/$d.img"; done
rm -f "$S"/zero1g.img
for v in 0 1 2; do mcopy -i "$S"/esp.img@@1M "$HERE/input-2h/vm$v.txt" "::/input/vm$v.txt"; done
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
mcopy -i "$S"/esp.img@@1M "$S"/hype.cfg ::/hype.cfg
cp /usr/share/edk2/ovmf/OVMF_VARS.fd "$S"/VARS.fd

LOG="$S"/serial.log
{
  # The i5 froze right after run2n's READBACK-MATCH, as its 64 MB bulk write started.
  for _ in $(seq 1 600); do
    sleep 1
    grep -aq "vm2 ttyS0| READBACK-MATCH" "$LOG" 2>/dev/null && break
  done
  echo "chord 1 at $(date +%T)" >&2
  printf 'sendkey ctrl_r-alt_r-1\n'; sleep 15
  printf 'sendkey ctrl_r-alt_r-d\n'; sleep 15
  printf 'sendkey ctrl_r-alt_r-1\n'; sleep 10
  printf 'sendkey ctrl_r-alt_r-d\n'; sleep 90
  printf 'quit\n'
} | timeout 1100 "$QEMU" -machine q35 -m 12288 -nodefaults \
  -accel kvm -cpu host -smp 16,sockets=1,cores=8,threads=2 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file="$S"/VARS.fd \
  -device ich9-ahci,id=ahci \
  -drive format=raw,file="$S"/esp.img,if=none,id=d0 \
  -device ide-hd,drive=d0,bus=ahci.0,bootindex=0 \
  -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 \
  -serial "file:$LOG" -monitor stdio -display none -vga std >"$S"/mon.log 2>"$S"/qemu.err || true

echo "=== build / vendor ==="
grep -a -m2 -E "^hype: build|vmm: (SVM|VMX) detected" "$LOG"
echo "PANIC=$(grep -a -c PANIC "$LOG")"
echo "=== scripts ==="
grep -a -E "SCRIPT vm[0-9]: (PASS|FAIL)" "$LOG" | cut -c1-160
grep -a -E "vm[0-9] ttyS0\| (READBACK-MATCH|BULK-[0-9]+|EXT4-WRITE-DONE|NTFS-WRITE-DONE|BOOT-OK-)" "$LOG" | cut -c1-120
echo "=== view switches ==="
grep -a -n "VIEWSWITCH" "$LOG" | cut -c1-140
first=$(grep -a -n -m1 "VIEWSWITCH: action" "$LOG" | cut -d: -f1)
if [ -z "$first" ]; then
  echo "INVALID: no chord reached hype (no VIEWSWITCH) -- nothing was tested"; exit 2
fi
after=$(awk -v n="$first" 'NR>n && /FBSPEED: t=/' "$LOG" | wc -l)
echo "FBSPEED lines after the first switch: $after"
grep -a -o -E "FBSPEED: t=[0-9]+ms" "$LOG" | tail -1
switches=$(grep -a -c "VIEWSWITCH: action" "$LOG")
if [ "$switches" -lt 4 ] || [ "$after" -lt 30 ]; then
  echo "FREEZE: switches=$switches, FBSPEED after first switch=$after [#708]"
  tail -40 "$LOG" | cut -c1-200
  exit 1
fi
echo "NO FREEZE: all $switches switches logged and the BSP kept running [#708]"
