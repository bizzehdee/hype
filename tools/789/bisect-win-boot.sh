#!/bin/sh
# #789: git-bisect test -- does the Windows install medium boot at all?
#
# The Win11 non-boot is the signal to bisect on rather than the Win10 stall: it is
# deterministic, it decides in about two minutes instead of forty, and "the firmware finds no
# boot option" is a far narrower symptom than "the kernel spins with no I/O".
#
# GOOD (exit 0) = the guest reached "Press any key to boot from CD or DVD".
# BAD  (exit 1) = "No bootable option or device was found".
# SKIP (exit 125) = the build failed, or neither line appeared, so this commit says nothing.
#
# The rig media is NOT rebuilt between steps. Only build/hype.efi changes, and run-win.sh
# re-copies it into the ESP every run -- rebuilding 6 GB per bisect step would make this
# untestable.
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
cd "$REPO"
LOG="$REPO/rig/bisect-win.log"

pkill -9 -x qemu-system-x86_64 2>/dev/null
sleep 1

make clean >/dev/null 2>&1
if ! make all >/dev/null 2>&1; then
    echo "SKIP: build failed at $(git rev-parse --short HEAD)"
    exit 125
fi

# Pristine guest firmware variables every step. They live in the ESP and persist across runs,
# so a previous step's boot entries would otherwise decide this one's verdict.
mcopy -i "$REPO/rig/media.img@@1M" -o "$REPO/fw/OVMF_VARS.fd" ::/EFI/hype/OVMF_VARS.fd
mcopy -i "$REPO/rig/media.img@@1M" -o "$REPO/tools/436/hype.cfg" ::/hype.cfg

SECS=${SECS:-200} LOG="$LOG" "$REPO/tools/436/run-win.sh" >/dev/null 2>&1

if LC_ALL=C grep -aq "Press any key to boot from CD or DVD" "$LOG" 2>/dev/null; then
    echo "GOOD: $(git rev-parse --short HEAD) reached the cdboot prompt"
    exit 0
fi
if LC_ALL=C grep -aq "No bootable option or device was found" "$LOG" 2>/dev/null; then
    echo "BAD: $(git rev-parse --short HEAD) found no boot option"
    exit 1
fi
echo "SKIP: $(git rev-parse --short HEAD) produced neither verdict"
exit 125
