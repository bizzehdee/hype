#!/bin/sh
# #436: boot hype from rig/media.img under QEMU and capture the run.
#
# Four rules encoded here, each learned from a wasted cycle:
#  - one QEMU at a time. Overlapping runs collide on the log and on killall, and
#    the second run's output silently interleaves into the first one's file.
#  - always under `timeout`. A wedged Windows guest never exits on its own.
#  - the HOST firmware is fw/OVMF_CODE.fd. The guest's pair is inside the ESP;
#    passing the guest's to QEMU (or vice versa) produces a 0-byte log and looks
#    exactly like hype crashing on entry.
#  - a real VGA device, never `-vga none`. Without it hype's display path never
#    runs at all, so a display bug cannot be observed and a working display
#    cannot be distinguished from an absent one.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
RIG="$REPO/rig"
SECS=${SECS:-240}
LOG=${LOG:-$RIG/hype.log}
SHOT=${SHOT:-$RIG/screen.ppm}

[ -f "$RIG/media.img" ] || { echo "rig/media.img missing -- run tools/436/make-win-rig.sh"; exit 1; }

# Refresh only hype.efi in the ESP: re-copying the 5.8 GB media per run would
# dominate the cycle, and the media does not change between builds.
if [ -f "$REPO/build/hype.efi" ]; then
    mcopy -i "$RIG/media.img@@1M" -o "$REPO/build/hype.efi" ::/EFI/BOOT/BOOTX64.EFI
fi
cp -f "$REPO/fw/OVMF_VARS.fd" "$RIG/host-vars.fd"

killall -9 qemu-system-x86_64 2>/dev/null || true
sleep 1
# Match the process NAME truncated to the 15 characters the kernel keeps in
# comm. `pgrep -x qemu-system-x86_64` matches nothing at all (the name is too
# long), and `pgrep -f` matches any shell whose command line merely mentions
# QEMU -- including the one running this script, which makes the guard fire on
# itself every time.
if pgrep -x qemu-system-x86 >/dev/null; then
    echo "a QEMU is still running -- refusing to start a second"; exit 1
fi

rm -f "$LOG" "$SHOT"
echo "running for up to ${SECS}s -> $LOG"
timeout "$SECS" qemu-system-x86_64 \
    -enable-kvm -cpu host -machine q35,accel=kvm -smp 4 -m 8192 \
    -drive if=pflash,format=raw,readonly=on,file="$REPO/fw/OVMF_CODE.fd" \
    -drive if=pflash,format=raw,file="$RIG/host-vars.fd" \
    -drive file="$RIG/media.img",format=raw,if=none,id=media \
    -device ahci,id=ahci \
    -device ide-hd,drive=media,bus=ahci.0,serial=HYPE436 \
    -vga std -display none \
    -serial "file:$LOG" \
    -qmp unix:"$RIG/qmp.sock",server,nowait \
    -no-reboot >"$RIG/qemu-stderr.txt" 2>&1 || true

killall -9 qemu-system-x86_64 2>/dev/null || true
echo "--- log tail ---"
LC_ALL=C tail -c 2000 "$LOG" 2>/dev/null || echo "(no log)"
