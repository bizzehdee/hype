#!/bin/bash
# #311/#166: boot FreeBSD 15.0 as a hype guest under QEMU+KVM (nested SVM) and capture
# hype's serial output. Usage: ./run-bsd.sh <log-name> [seconds]
#
# The ESP image (qbsd.img) is an mtools-built FAT image, NOT vvfat -- vvfat SIGSEGVs QEMU
# inside its own AHCI emulation while OVMF reads it (#288), which reads as a hype bug and
# is not one. Only BOOTX64.EFI is refreshed per run; the 1.3GB ISO is left in place.
set -e
cd "$(dirname "$0")/../../disk-images"
LOG="${1:-bsd-run}.log"
SECS="${2:-180}"
OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/ovmf/OVMF_CODE.fd}"
OVMF_VARS="${OVMF_VARS:-/usr/share/edk2/ovmf/OVMF_VARS.fd}"

[ -f ../build/hype.efi ] || { echo "build/hype.efi missing -- run make all"; exit 1; }
mcopy -i qbsd.img -o ../build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
# The HOST varstore must be the pair of the HOST OVMF_CODE. Handing QEMU hype's own vendored
# GUEST varstore (fw/OVMF_VARS.fd, 540672 bytes, for the 4MB build) instead pairs a mismatched
# size against a 2MB CODE and the boot produces NO serial output at all -- a silent 0-byte log
# that looks exactly like hype crashing on entry. Copied fresh per run so a previous run's
# BootOrder cannot change what gets booted.
cp -f "$OVMF_VARS" host-vars.fd
echo "hype.efi refreshed in ESP; booting for ${SECS}s -> $LOG"

rm -f "$LOG"
qemu-system-x86_64 \
  -machine q35 -m 8192 -nodefaults \
  -accel kvm -cpu host -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file=host-vars.fd \
  -drive format=raw,file=qbsd.img \
  -serial "file:$LOG" -display none -vga none 2>qemu-stderr.txt &
QPID=$!
# Bound by wall clock: the guest never exits on its own, and a hung run must still yield a log.
for _ in $(seq "$SECS"); do
    kill -0 $QPID 2>/dev/null || break
    sleep 1
done
kill -9 $QPID 2>/dev/null || true
wait $QPID 2>/dev/null || true
echo "done: $(wc -c < "$LOG") bytes in $LOG"
