#!/bin/sh
# #789: run one hype config against the rig media and report how far the guest gets.
#
# The question this answers is narrow: for a given hype.cfg, how many ATAPI READ(10)s does the
# guest issue before it stops asking, and what does the guest firmware say about the boot option.
# Everything else is the #695 runner's environment, unchanged -- including the loop-mount guard,
# because contaminating media.img produced three false "No bootable option" runs on this ticket.
#
# Usage: CFG=tools/436/hype.cfg LOG=rig/foo.log SECS=300 tools/789/run-789.sh
set -eu
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
RIG="$REPO/rig"
CFG=${CFG:?set CFG to the hype.cfg to stage}
SECS=${SECS:-300}
LOG=${LOG:-$RIG/i789.log}
SHOT=${SHOT:-$RIG/i789-screen.ppm}

[ -f "$RIG/media.img" ] || { echo "rig/media.img missing -- run tools/436/make-win-rig.sh"; exit 1; }
[ -f "$REPO/$CFG" ] || [ -f "$CFG" ] || { echo "no such config: $CFG"; exit 1; }
if pidof qemu-system-x86_64 >/dev/null; then
    echo "a QEMU is still running -- refusing to start a second"; exit 1
fi
if findmnt -n -o SOURCE | grep -q "^/dev/loop[0-9]*p"; then
    echo "REFUSING: a loop partition is mounted on this host:"
    findmnt -n -o SOURCE,TARGET | grep "^/dev/loop[0-9]*p"
    exit 1
fi
if losetup -a 2>/dev/null | grep -q "$RIG/media.img"; then
    echo "REFUSING: rig/media.img is still loop-mapped:"; losetup -a | grep "$RIG/media.img"; exit 1
fi

mcopy -i "$RIG/media.img@@1M" -o "$REPO/build/hype.efi" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$RIG/media.img@@1M" -o "$CFG" ::/hype.cfg
mcopy -i "$RIG/media.img@@1M" -o "$REPO/tools/436/input-vm0.txt" ::/input/vm0.txt
cp -f "$REPO/fw/OVMF_VARS.fd" "$RIG/host-vars.fd"
rm -f "$LOG" "$SHOT" "$RIG/qmp789.sock"

echo "config=$CFG  for up to ${SECS}s -> $LOG"
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
    -qmp unix:"$RIG/qmp789.sock",server,nowait \
    >"$RIG/i789-stderr.txt" 2>&1 &
QPID=$!

# A screendump near the end of the window. The guest console goes quiet once Windows leaves text
# mode, so after that point the framebuffer is the only evidence of what it is doing -- and a run
# that ends with no picture cannot distinguish "still working" from "stopped".
i=0
while [ "$i" -lt "$((SECS - 15))" ] && kill -0 "$QPID" 2>/dev/null; do sleep 5; i=$((i + 5)); done
if kill -0 "$QPID" 2>/dev/null; then
    printf '{"execute":"qmp_capabilities"}\n{"execute":"screendump","arguments":{"filename":"%s"}}\n' \
        "$SHOT" | timeout 20 socat - UNIX-CONNECT:"$RIG/qmp789.sock" >/dev/null 2>&1 || true
fi
wait "$QPID" 2>/dev/null || true

[ -s "$SHOT" ] && echo "screenshot: $SHOT"
echo "--- what the guest firmware decided ---"
LC_ALL=C grep -a "BdsDxe: \(loading\|starting\|failed\|No bootable\)" "$LOG" | sed 's/.*| //' | sort -u
echo "--- how far the ATAPI read stream got ---"
LC_ALL=C grep -a "ATAPIOPS vm0:" "$LOG" | tail -1
LC_ALL=C grep -a "DIAG: ATAPI READ(10)" "$LOG" | tail -1
echo "--- where each vCPU ended up ---"
LC_ALL=C grep -a "GUESTPC vm0:" "$LOG" | tail -1 | sed 's/ | .*//'
LC_ALL=C grep -a "APVCPU vm0/[1-9]:" "$LOG" | tail -1 | sed 's/ ahci=.*//'
