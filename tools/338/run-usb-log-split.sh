#!/bin/bash
# #338 validation: boot hype FROM an emulated USB stick, then read back the split log files the
# run leaves on it.
#
# #807 -- WHY THIS BOOTS FROM USB AND MUST KEEP DOING SO.
#
# This rig used to boot hype from an IDE ESP and offer the log target as a SEPARATE USB stick.
# #638 then made the log sink require that the volume be the one hype itself booted from:
#
#   "a mountable FAT32 volume is not enough -- it must be the volume hype itself booted from.
#    MSC enumeration order is an accident of USB topology; the operator pulls the drive hype
#    booted from to read a run's logs, so a bystander stick must be left alone rather than
#    silently claimed for HYPE.LOG."          -- boot/main.c, usb_base_is_boot_volume()
#
# That is correct, and it made this rig's premise -- boot from one device, log to another --
# exactly what the policy forbids. The rig kept exiting 0 and testing NOTHING: every run showed
# `usb-log: ... not this device's role [#638]`, `USBFLUSH slices=0 drained=0B`, and an empty
# stick. It went unnoticed because it is the ONLY rig where the USB log sink can mount at all
# (tools/run-guest.sh silently no-ops anything gated on it), so its silence looked like nothing
# to see. Four separate changes shipped unverifiable because of it -- #806's flush verb, #808's
# varstore fix, and two log-integrity questions in #809.
#
# So: ONE USB disk, GPT-partitioned, holding the ESP hype boots from, and the log lands on that
# same volume. Which is exactly what the real hardware-validation drive is, and what this rig
# should have modelled from the start.
#
# Two traps this encodes, both learned elsewhere in the tree:
#   * GPT, never a bare-FAT superfloppy. hype's resolver walks GPT partitions 1-4
#     (hype_gpt_find_partition) before handing the volume to core/fat.c, so a bare `mkfs.vfat`
#     image has no partition for it to find -- see tools/run-guest.sh's build_esp_file().
#   * FULLY ALLOCATED, not sparse. A sparse image handed to QEMU in the same breath
#     intermittently comes up as `BdsDxe: ... Not Found` with no boot option at all.
#
#   tools/338/run-usb-log-split.sh [seconds]        default 150
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-$(mktemp -d)}"
B=build
SECS="${1:-150}"
ISO="${ISO:-disk-images/alpine-hype-dbg.iso}"
[ -f "$ISO" ] || ISO="$(ls disk-images/*.iso | head -1)"
mkdir -p "$S"
killall -9 "$(basename "$QEMU")" 2>/dev/null || true; sleep 1

# One disk: GPT, partition 1 = the FAT32 volume hype boots from AND logs to.
ISO_MB=$(( $(stat -c%s "$ISO") / 1048576 + 1 ))
PART_MB=$(( ISO_MB + 160 ))          # ISO + firmware + room for the logs this rig exists to read
rm -f "$S/usb.img" "$S/part1.img"
fallocate -l "$(( PART_MB + 2 ))M" "$S/usb.img" 2>/dev/null || \
    dd if=/dev/zero of="$S/usb.img" bs=1M count=$(( PART_MB + 2 )) status=none conv=fsync
sfdisk --label gpt -q "$S/usb.img" >/dev/null <<SFDISK
2048,$(( PART_MB * 2048 )),U
SFDISK

# Built standalone then written into place: mtools addresses a whole image, not a partition.
fallocate -l "${PART_MB}M" "$S/part1.img" 2>/dev/null || \
    dd if=/dev/zero of="$S/part1.img" bs=1M count="$PART_MB" status=none
mkfs.vfat -F 32 -n HYPEUSB "$S/part1.img" >/dev/null
mmd -i "$S/part1.img" ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso
mcopy -i "$S/part1.img" $B/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$S/part1.img" fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i "$S/part1.img" "$ISO" ::/iso/test.iso
mcopy -i "$S/part1.img" tools/qemu-cd-hype.cfg ::/hype.cfg
dd if="$S/part1.img" of="$S/usb.img" bs=1M seek=1 conv=notrunc,fsync status=none
sync "$S/usb.img"

# Verify the image before spending a boot on it -- a short write here reads as hype failing.
mdir -i "$S/usb.img@@1M" ::/EFI/BOOT | grep -q BOOTX64 || {
    echo "image verify FAILED: BOOTX64.EFI not in partition 1"; exit 1; }

for ATTEMPT in 1 2 3; do
  cp /usr/share/OVMF/OVMF_VARS.fd "$S/VARS.fd"
  timeout "$SECS" "$QEMU" \
    -machine q35 -m 2048 -nodefaults \
    -accel kvm -accel tcg -cpu host -smp 2 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
    -drive if=pflash,format=raw,file="$S/VARS.fd" \
    -device qemu-xhci,id=xhci \
    -drive format=raw,file="$S/usb.img",if=none,id=stick \
    -device usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    -serial stdio -display none -vga none > "$S/serial-$ATTEMPT.txt" 2>&1 || true
  if grep -aq "hype: build" "$S/serial-$ATTEMPT.txt"; then
    echo "attempt $ATTEMPT: hype ran"; cp "$S/serial-$ATTEMPT.txt" "$S/serial.txt"; break
  fi
  echo "attempt $ATTEMPT: hype never ran (#371 noboot) -- retrying"
done

echo "=== verdict ==="
rc=0
# The whole point: the sink must MOUNT. A rejected volume is the #807 regression returning.
if grep -aq "not this device's role" "$S/serial.txt"; then
  echo "FAIL: the log volume was REJECTED -- hype did not boot from the volume it is logging to"
  grep -a "usb-log:" "$S/serial.txt" | head -3
  rc=1
fi
grep -a "usb-log: split diagnostics" "$S/serial.txt" | tail -1 || true
DRAINED=$(grep -ao "drained=[0-9]*B" "$S/serial.txt" | tail -1)
echo "flush: ${DRAINED:-none}"
case "${DRAINED:-drained=0B}" in
  drained=0B) echo "FAIL: nothing was ever written to the sink"; rc=1 ;;
esac
echo "=== files left on the drive ==="
mdir -i "$S/usb.img@@1M" :: || true
for f in HYPE.LOG CDTEST.LOG; do
  sz=$(mdir -i "$S/usb.img@@1M" :: 2>/dev/null | awk -v n="${f%%.*}" '$1==n {print $3}')
  [ -n "$sz" ] && echo "  $f = $sz bytes"
done
[ "$rc" -eq 0 ] && echo "PASS: hype booted from the USB volume and wrote its log to it"
exit "$rc"
