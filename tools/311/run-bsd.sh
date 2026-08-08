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

# #343: SCORE THE RUN, on both axes, and say so.
#
# Every FreeBSD run was graded on `bsdinstall` being reached -- which it is in BOTH outcomes of
# #343, because the intermittent kernel page fault happens after that point. So the pass/fail
# signal this rig produced was blind to a guest panic, and #343 surfaced only because #342 needed
# a fault grep to rule out a regression. While that is open, this rig is hype's most-used
# regression gate, so a real regression would be indistinguishable from #343's coin flip.
#
# Also print the fault detail. The panic lines carry the guest's own RIP, CR2 and fault address,
# and the whole reason #343 has no root cause yet is that nobody read them before the logs were
# lost -- resolving that RIP against the FreeBSD kernel's symbols is what cracked #311. Surfacing
# them automatically means the next run cannot leave them unread.
reached=0
panicked=0
grep -aq "bsdinstall" "$LOG" && reached=1
grep -aqE "panic:|Fatal trap" "$LOG" && panicked=1

echo "score: reached_bsdinstall=$reached guest_panic=$panicked"
if [ "$panicked" = 1 ]; then
    echo "---- guest fault detail (#343: resolve this RIP against the FreeBSD kernel symbols) ----"
    grep -aE -A 12 "Fatal trap" "$LOG" | head -40
    echo "----------------------------------------------------------------------------------------"
fi

# Non-zero on either failure, so this can be used as a gate rather than eyeballed. A guest panic
# is a FAILURE even though bsdinstall was reached: that combination is exactly #343.
[ "$reached" = 1 ] || { echo "FAIL: never reached bsdinstall"; exit 1; }
[ "$panicked" = 0 ] || { echo "FAIL: guest panicked (#343 while it is open, a regression once it is not)"; exit 1; }
echo "PASS: reached bsdinstall, no guest panic"
