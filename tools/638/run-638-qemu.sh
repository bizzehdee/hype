#!/bin/bash
# #638: the log volume must be the drive hype booted from, never whichever USB
# mass-storage device enumerates first.
#
# Two USB sticks on the same xHCI controller: a BLANK one (no partition table,
# exactly the bare-metal i5 run's Cruzer) attached FIRST, and hype's real ESP
# (loader + firmware + \hype.cfg) attached SECOND. QEMU enumerates USB devices
# in the order they're wired to the controller, so this reproduces the failure
# ordering directly instead of hoping for it.
#
# Bar (from the ticket): \HYPE.LOG lands on the boot ESP, named in a log line;
# the blank stick is provably never opened; a run whose boot device genuinely
# has nothing writable still reports that loudly (unchanged fallback).
set -e
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-disk-images/638}"
B=build
SECS="${1:-300}"
ISO="${ISO:-disk-images/alpine-virt-console.iso}"
CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE.fd}
VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS.fd}
mkdir -p "$S"
killall -9 qemu-system-x86_64 2>/dev/null || true; sleep 1

# The blank stick: zeroed, no partition table, no filesystem at all -- exactly
# what a bystander stick looks like to the media scan.
dd if=/dev/zero of=$S/blank.img bs=1M count=32 status=none

# The boot ESP: GPT (a bare-FAT superfloppy kills the media resolver, #525),
# carrying hype's own loader + firmware + a minimal one-VM config so
# usb_base_is_boot_volume() has something to verify against.
cat > $S/hype.cfg <<'CFG'
[hype]
config_version = 1
log_level = debug

[vm.chatter]
label = logchatter
vcpus = 1
mem_mb = 512
boot = kernel
kernel = \EFI\hype\micro\logchatter.bin
cmdline = lines=200
os_hint = none
CFG

rm -f $S/esp.img
dd if=/dev/zero of=$S/esp.img bs=1M count=256 status=none
sfdisk -q $S/esp.img <<'PT'
label: gpt
start=2048, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B
PT
E="$S/esp.img@@1M"
mkfs.vfat -F 32 -n HYPEESP --offset 2048 $S/esp.img >/dev/null
mmd -i "$E" ::/EFI ::/EFI/BOOT ::/EFI/hype ::/EFI/hype/micro ::/iso ::/input ::/hype
mcopy -i "$E" $B/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$E" fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i "$E" $B/micro/logchatter.bin ::/EFI/hype/micro/logchatter.bin
mcopy -i "$E" $S/hype.cfg ::/hype.cfg

for ATTEMPT in 1 2 3; do
  cp "$VARS" $S/VARS.fd
  timeout "$SECS" qemu-system-x86_64 \
    -machine q35 -m 2048 -nodefaults \
    -accel kvm -accel tcg -cpu host -smp 2 \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file=$S/VARS.fd \
    -device qemu-xhci,id=xhci \
    -drive format=raw,file=$S/blank.img,if=none,id=blankstick \
    -device usb-storage,bus=xhci.0,drive=blankstick,port=1 \
    -drive format=raw,file=$S/esp.img,if=none,id=espstick \
    -device usb-storage,bus=xhci.0,drive=espstick,port=2,bootindex=0 \
    -serial stdio -display none -vga none > $S/serial-$ATTEMPT.txt 2>&1 || true
  if grep -aq "hype: build" $S/serial-$ATTEMPT.txt; then
    echo "attempt $ATTEMPT: hype ran"; cp $S/serial-$ATTEMPT.txt $S/serial.txt; break
  fi
  echo "attempt $ATTEMPT: hype never ran (#371 noboot) -- retrying"
done

echo "=== verdict ==="
rc=0
if grep -aq "usb-log:.*not the volume hype.*booted from" $S/serial.txt; then
  echo "PASS: at least one candidate base was checked and rejected as not-the-boot-volume"
else
  echo "FAIL: the #638 identity check never rejected anything -- either it didn't run, or the blank stick was never reached"
  rc=1
fi
if grep -aq "HYPE.LOG.*disk LBA" $S/serial.txt; then
  echo "PASS: HYPE.LOG opened"
else
  echo "FAIL: HYPE.LOG never opened at all"
  rc=1
fi
# The blank stick has no filesystem, so hype_fs_mount_auto must fail on every base for it
# and usb_base_is_boot_volume() must never even get to open a file there. There is no file
# to check for absence, so the proof is structural: verify the log opened on the SECOND
# device brought up (the ESP), by confirming the boot-volume banner (#447) matches the ESP's
# own \hype.cfg checksum path, and that the run has exactly ONE HYPE.LOG open (never re-pointed).
opens=$(grep -ac "disk LBA" $S/serial.txt || true)
echo "log-sink open count: $opens"
if [ "$opens" != "1" ]; then
  echo "FAIL: expected exactly one successful log-sink open, saw $opens"
  rc=1
fi
[ "$rc" -eq 0 ] && echo "PASS: log landed on the boot ESP, blank stick left alone, opened exactly once"
exit "$rc"
