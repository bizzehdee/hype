#!/bin/bash
# #596: reproduce the log-writer FAT32 corruption in QEMU.
#
# Boots hype from an ESP with the FAT32 self-test config (3 concurrent logchatter guests) and a
# large FAT32 USB stick as the LOG volume -- the one rig where g_hype_log mounts (#338). The
# markers \F32TEST.RUN + \LOGTEST.RUN on the USB volume arm the batteries, and hype's real log
# writer grows \HYPE.LOG + \VM*.LOG concurrently on it during the guest run. QEMU's usb-storage
# honours SYNCHRONIZE CACHE, so hype's #377 barrier is ACTIVE -- matching the SATA-USB SSD where
# #596 reproduced. Then fsck.vfat judges the USB image: a "File size ... cluster chain length"
# (dirent > chain) line is the bug.
set -e
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-disk-images/596-qemu}"
B=build
mkdir -p "$S"
CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE.fd}
VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS.fd}
ITERS=${ITERS:-3}

killall -9 qemu-system-x86_64 2>/dev/null || true; sleep 1

# --- the LOG volume (where g_hype_log mounts): large, 32 KiB clusters, both markers armed ---
rm -f "$S/usb.img"
truncate -s 2G "$S/usb.img"
mkfs.vfat -F 32 -s 64 -n HYPEUSB "$S/usb.img" >/dev/null
printf '%s\n' "$ITERS" | mcopy -i "$S/usb.img" - ::F32TEST.RUN
printf '%s\n' "$ITERS" | mcopy -i "$S/usb.img" - ::LOGTEST.RUN

# --- the boot ESP: hype.efi + firmware + the fat32 config + every micro kernel ---
rm -f "$S/esp.img"
dd if=/dev/zero of="$S/esp.img" bs=1M count=256 status=none
mkfs.vfat -F 32 -n HYPEESP "$S/esp.img" >/dev/null
mmd -i "$S/esp.img" ::/EFI ::/EFI/BOOT ::/EFI/hype ::/EFI/hype/micro ::/iso
mcopy -i "$S/esp.img" "$B/hype.efi" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$S/esp.img" fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i "$S/esp.img" "$B"/micro/*.bin ::/EFI/hype/micro/
mcopy -i "$S/esp.img" tools/hwstick/hype-fat32.cfg ::/hype.cfg

for ATTEMPT in 1 2 3; do
    cp "$VARS" "$S/VARS.fd"
    timeout "${1:-400}" qemu-system-x86_64 \
        -machine q35 -m 4096 -nodefaults \
        -accel kvm -accel tcg -cpu host -smp 4 \
        -drive if=pflash,format=raw,readonly=on,file="$CODE" \
        -drive if=pflash,format=raw,file="$S/VARS.fd" \
        -drive format=raw,file="$S/esp.img",if=none,id=esp \
        -device ide-hd,drive=esp,bus=ide.0,bootindex=0 \
        -device qemu-xhci,id=xhci \
        -drive format=raw,file="$S/usb.img",if=none,id=stick \
        -device usb-storage,bus=xhci.0,drive=stick \
        -serial stdio -display none -vga none > "$S/serial-$ATTEMPT.txt" 2>&1 || true
    if grep -aq "hype: build" "$S/serial-$ATTEMPT.txt"; then
        echo "attempt $ATTEMPT: hype ran"; cp "$S/serial-$ATTEMPT.txt" "$S/serial.txt"; break
    fi
    echo "attempt $ATTEMPT: hype never ran -- retrying"
done

echo "=== verdicts ==="
grep -aE 'FAT32-STICK SELFTEST|LOGTEST-STICK SELFTEST' "$S/serial.txt" || echo "(no verdict -- did the batteries run?)"
echo "=== fsck.vfat -n on the USB log volume (the judge) ==="
fsck.vfat -n "$S/usb.img" 2>&1 | grep -aiE 'chain length|beyond EOF|Reclaimed|lost|orphan|free cluster summary|Bad|allocation|clean|files,' || true
echo "--- files ---"; mdir -i "$S/usb.img" ::/ 2>/dev/null | head
