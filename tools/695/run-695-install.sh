#!/bin/sh
# #695: run the unattended Windows install under hype, to produce the reusable guest image.
#
# Not tools/436/run-win.sh, for one reason that would end the run early: that script passes
# -no-reboot, which is right for a bring-up rig watching a single boot and wrong here. Windows
# Setup reboots two or three times between applying the image and reaching the desktop, and if
# hype passes any of those through, QEMU exits and the install is lost at whatever stage it had
# reached.
#
# Everything else is deliberately the same as the #436 rig, including the guest firmware pair
# inside the ESP and the host's own OVMF passed separately -- mixing those two produces a
# 0-byte log that looks exactly like hype crashing on entry.
set -eu
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
RIG="$REPO/rig"
SECS=${SECS:-5400}
LOG=${LOG:-$RIG/win695.log}
SHOT=${SHOT:-$RIG/win695-screen.ppm}

[ -f "$RIG/media.img" ] || { echo "rig/media.img missing -- run tools/436/make-win-rig.sh"; exit 1; }
if pidof qemu-system-x86_64 >/dev/null; then
    echo "a QEMU is still running -- refusing to start a second"; exit 1
fi

# Nothing of the host's may be attached to the image QEMU is about to open read-write.
#
# This cost three runs and a wrong conclusion on #789. Creating a loop device on media.img
# makes udisks AUTO-MOUNT its partitions -- the ISO partition as UDF, the data partition as
# exFAT -- so the kernel is caching and writing filesystem metadata into the same file the
# guest is booting from. The symptom was "No bootable option or device was found", which reads
# exactly like a firmware or media fault and is neither. `udisksctl loop-delete` then fails
# silently while a partition is still mounted, so the loops accumulate run on run.
if findmnt -n -o SOURCE | grep -q "^/dev/loop[0-9]*p"; then
    echo "REFUSING: a loop partition is mounted on this host:"
    findmnt -n -o SOURCE,TARGET | grep "^/dev/loop[0-9]*p"
    echo "unmount it and delete the loop before running -- see the note above"
    exit 1
fi
if losetup -a 2>/dev/null | grep -q "$RIG/media.img"; then
    echo "REFUSING: rig/media.img is still loop-mapped:"
    losetup -a | grep "$RIG/media.img"
    exit 1
fi

# Refresh the mutable ESP files; the 6.1 GB media partition is left alone.
mcopy -i "$RIG/media.img@@1M" -o "$REPO/build/hype.efi" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$RIG/media.img@@1M" -o "$REPO/tools/695/hype.cfg" ::/hype.cfg
mcopy -i "$RIG/media.img@@1M" -o "$REPO/tools/436/input-vm0.txt" ::/input/vm0.txt
cp -f "$REPO/fw/OVMF_VARS.fd" "$RIG/host-vars.fd"
rm -f "$LOG" "$SHOT" "$RIG/qmp695.sock"

echo "installing for up to ${SECS}s -> $LOG"
timeout "$SECS" "$QEMU" \
    -enable-kvm -cpu host -machine q35,accel=kvm -smp 4 -m 8192 \
    -drive if=pflash,format=raw,readonly=on,file="$REPO/fw/OVMF_CODE.fd" \
    -drive if=pflash,format=raw,file="$RIG/host-vars.fd" \
    -drive file="$RIG/media.img",format=raw,if=none,id=media \
    -device ahci,id=ahci \
    -device ide-hd,drive=media,bus=ahci.0,serial=HYPE436 \
    -device qemu-xhci,id=hostxhci \
    -device usb-mouse,bus=hostxhci.0 \
    -vga std -display none \
    -serial "file:$LOG" \
    -qmp unix:"$RIG/qmp695.sock",server,nowait \
    >"$RIG/win695-stderr.txt" 2>&1 || true

echo "--- log tail ---"
LC_ALL=C tail -c 1500 "$LOG" 2>/dev/null || echo "(no log)"
