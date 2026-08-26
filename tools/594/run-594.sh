#!/bin/bash
# #594 leg: a kernel VM with bus = usb-msc. PASS = a real Linux binds usb-storage over hype's
# emulated xHCI, /dev/sda is removable, and its LBA0 reads hype's on-disk magic byte-exact.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
cd "$(git rev-parse --show-toplevel)"
killall -9 "$(basename "$QEMU")" 2>/dev/null || true
sleep 1
HYPE_CFG=tools/594/hype.cfg \
HYPE_KERNELS="disk-images/545/vmlinuz-virt disk-images/545/initramfs-virt" \
HYPE_DISK="disk-images/594/usbdisk.img" \
HYPE_INPUT=tools/594/shell-vm0.txt \
    tools/run-guest.sh disk-images/alpine-hype-dbg.iso 594-usb-msc "${TIMEOUT:-300}" || true
LOG=disk-images/run-594-usb-msc.log
echo "=== hype presented the xHCI + MSC? ==="
LC_ALL=C grep -aE "guest xHCI controller presented|removable USB-MSC attached|#591 .*window latched|xHCI BAR0 enabled" "$LOG" | head
echo "=== guest USB enumeration ==="
LC_ALL=C grep -aE "xhci_hcd|usb 1-|usb-storage|Direct-Access|removable|sda|scsi" "$LOG" | grep -a "vm0 ttyS0" | tail -20
echo "=== verdict ==="
LC_ALL=C grep -aE "FOUND-/dev/sda|RMB-DONE|USB594-|SCRIPT vm0: (PASS|FAIL)" "$LOG" | grep -av "screen|" | head
LC_ALL=C grep -a "HYPE594-USB-MSC" "$LOG" | head -2
LC_ALL=C grep -aq "SCRIPT vm0: PASS" "$LOG" && echo "PASS: removable USB-MSC read by a real guest" || { echo "FAIL"; exit 1; }
