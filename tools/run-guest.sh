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

build_esp() {
    # ISO + firmware + slack, rounded up to whole MB.
    local mb
    mb=$(( $(stat -c%s "$ISO") / 1048576 + 96 ))
    rm -f "$ESP"
    # FULLY ALLOCATED, not `truncate`. tools/make-disk-image.sh explains the general reason;
    # the reason here is narrower and was learned the hard way: a sparse ESP built and handed
    # to QEMU in the same breath intermittently came up as `BdsDxe: ... Not Found` / no boot
    # option at all, on an image mtools itself could list correctly. Allocating and then
    # syncing removes the variable rather than leaving a retry to paper over it.
    fallocate -l "${mb}M" "$ESP" 2>/dev/null || \
        dd if=/dev/zero of="$ESP" bs=1048576 count="$mb" status=none conv=fsync
    mformat -i "$ESP" -F ::
    mmd -i "$ESP" ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso
    mcopy -i "$ESP" build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
    mcopy -i "$ESP" fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
    mcopy -i "$ESP" "$ISO" ::/iso/test.iso
    sync "$ESP"

    # Verify what was produced rather than trusting the tools -- the same discipline
    # tools/make-disk-image.sh applies, for the same reason: a silent short write here looks
    # exactly like hype failing to boot.
    local want have
    want=$(stat -c%s "$ISO")
    have=$(mdir -i "$ESP" ::/iso 2>/dev/null | awk '/test *iso/ {print $3}')
    [ "$have" = "$want" ] || { echo "ESP verify FAILED: test.iso is $have bytes, wanted $want"; return 1; }
    mdir -i "$ESP" ::/EFI/BOOT 2>/dev/null | grep -q BOOTX64 || \
        { echo "ESP verify FAILED: BOOTX64.EFI missing"; return 1; }
    return 0
}

boot_once() {
    cp -f "$OVMF_VARS" "$OUT.vars.fd"   # fresh, so a previous run's BootOrder cannot decide
    rm -f "$LOG"
    qemu-system-x86_64 \
      -machine q35 -m 8192 -nodefaults \
      -accel kvm -cpu host -smp 4 \
      -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
      -drive if=pflash,format=raw,file="$OUT.vars.fd" \
      -drive format=raw,file="$ESP" \
      -serial "file:$LOG" -display none -vga none 2>"$OUT.stderr" &
    local qpid=$!
    # Bounded by wall clock: these guests never exit on their own, and a hung run must still
    # leave a log behind to read.
    local i
    for i in $(seq "$SECS"); do kill -0 $qpid 2>/dev/null || break; sleep 1; done
    kill -9 $qpid 2>/dev/null || true
    wait $qpid 2>/dev/null || true
}

build_esp || exit 1
echo "booting $(basename "$ISO") for ${SECS}s -> $LOG"
boot_once
# hype prints its own banner as soon as it is entered. Its absence means the firmware never
# handed off, which is a harness failure and NOT a hype result -- say so instead of letting a
# near-empty log be read as "hype produced nothing".
if ! grep -aq "^hype: build" "$LOG"; then
    echo "WARNING: firmware never launched hype (no banner) -- rebuilding the ESP and retrying once"
    build_esp || exit 1
    boot_once
    grep -aq "^hype: build" "$LOG" || echo "WARNING: still no hype banner -- treat this log as a HARNESS failure"
fi
echo "done: $(wc -c < "$LOG") bytes in $LOG"
