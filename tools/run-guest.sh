#!/bin/bash
# Boot an arbitrary ISO as a hype guest under QEMU+KVM (nested SVM) and capture hype's serial.
#
#   tools/run-guest.sh <iso> <log-name> [seconds]
#
# Builds a throwaway FAT ESP image sized to the ISO, drops in the freshly built hype.efi plus
# hype's vendored guest firmware, and boots it. Used for the #166 FreeBSD work and for the
# Alpine regression check that any change to the shared interrupt-injection path needs.
#
# Two traps this encodes, both of which cost a wasted run before being understood:
#
#  * The ESP must be an mtools-built FAT **image**, never QEMU's vvfat. vvfat SIGSEGVs QEMU
#    inside its own AHCI emulation while OVMF reads it (#288) -- which reads as a hype crash
#    and is not one.
#  * The HOST pflash varstore must be the pair of the HOST OVMF_CODE. Handing QEMU hype's own
#    vendored GUEST varstore (fw/OVMF_VARS.fd, for the 4MB build) against a 2MB host CODE
#    produces a 0-byte serial log, indistinguishable from hype faulting on entry.
set -e
cd "$(dirname "$0")/.."
ISO="$1"; NAME="${2:-guest}"; SECS="${3:-180}"
[ -n "$ISO" ] && [ -f "$ISO" ] || { echo "usage: $0 <iso> <log-name> [seconds]"; exit 1; }
[ -f build/hype.efi ] || { echo "build/hype.efi missing -- run make all"; exit 1; }

OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/ovmf/OVMF_CODE.fd}"
OVMF_VARS="${OVMF_VARS:-/usr/share/edk2/ovmf/OVMF_VARS.fd}"
OUT=disk-images/run-$NAME
LOG="$OUT.log"
ESP="$OUT.esp.img"

# ISO + firmware + slack, rounded up to whole MB.
MB=$(( $(stat -c%s "$ISO") / 1048576 + 96 ))
rm -f "$ESP"
truncate -s "${MB}M" "$ESP"
mformat -i "$ESP" -F ::
mmd -i "$ESP" ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso
mcopy -i "$ESP" build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP" fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i "$ESP" "$ISO" ::/iso/test.iso
cp -f "$OVMF_VARS" "$OUT.vars.fd"

echo "booting $(basename "$ISO") for ${SECS}s -> $LOG"
rm -f "$LOG"
qemu-system-x86_64 \
  -machine q35 -m 8192 -nodefaults \
  -accel kvm -cpu host -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OUT.vars.fd" \
  -drive format=raw,file="$ESP" \
  -serial "file:$LOG" -display none -vga none 2>"$OUT.stderr" &
QPID=$!
# Bounded by wall clock: the guest never exits on its own, and a hung run must still yield a log.
for _ in $(seq "$SECS"); do kill -0 $QPID 2>/dev/null || break; sleep 1; done
kill -9 $QPID 2>/dev/null || true
wait $QPID 2>/dev/null || true
echo "done: $(wc -c < "$LOG") bytes in $LOG"
