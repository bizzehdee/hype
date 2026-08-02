#!/bin/sh
# #262: run the rig built by make-rig.sh through hype and report the verdict.
# Rebuild hype and re-stage its binary into the ESP image before each run --
# forgetting that step silently re-tests the previous build.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=${OUT:-/mnt/data/hype-bisect/rig262}
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS.fd}
cd "$OUT"

mcopy -o -i esp-hype.fat "$REPO/build/hype.efi" ::/EFI/BOOT/BOOTX64.EFI
dd if=esp-hype.fat of=esp-hype.img bs=512 seek=2048 conv=notrunc status=none

killall -9 qemu-system-x86_64 2>/dev/null || true
sleep 1
rm -f hype.log
cp "$OVMF_VARS" hype-vars.fd
timeout "${RUNSECS:-200}" qemu-system-x86_64 -machine q35 -m 8192 -nodefaults \
  -accel kvm -cpu host -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file=hype-vars.fd \
  -drive format=raw,file=esp-hype.img,if=none,id=esp -device ide-hd,drive=esp,bus=ide.0 \
  -serial file:hype.log -display none 2>/dev/null || true

echo "== disk the guest was given =="
tr -d '\r' < hype.log | grep -E "m5-8:|#262 SATA disk attached" || echo "   (none -- check #285: wrong path yields the RAM scratch)"
echo "== ATA commands the guest firmware actually issued =="
tr -d '\r' < hype.log | grep -E "#262 ATACMD" || echo "   NONE -- firmware never issued a command"
echo "== verdict =="
if tr -d '\r' < hype.log | grep -q "Shell>"; then
    echo "   PASS -- guest firmware booted the SATA disk"
else
    tr -d '\r' < hype.log | grep -E "No bootable option" | head -1
    echo "   FAIL -- guest firmware found nothing to boot"
fi
