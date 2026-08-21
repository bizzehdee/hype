#!/bin/bash
# #588 leg: a boot = kernel VM WITH a virtio-blk disk. PASS = the guest reaches its shell
# (did not wedge at the virtio probe) and the disk function's BAR reads back programmed.
set -e
cd "$(git rev-parse --show-toplevel)"
killall -9 qemu-system-x86_64 2>/dev/null || true
sleep 1
HYPE_CFG=tools/588/hype.cfg \
HYPE_KERNELS="disk-images/545/vmlinuz-virt disk-images/545/initramfs-virt" \
HYPE_DISK="disk-images/588/vda.img" \
HYPE_INPUT=tools/588/shell-vm0.txt \
    tools/run-guest.sh disk-images/alpine-hype-dbg.iso 588-kernel-disk "${TIMEOUT:-300}" || true
LOG=disk-images/run-588-kernel-disk.log
echo "=== #588 programmed BARs (hype side) ==="
LC_ALL=C grep -a "#588 programmed dev" "$LOG" | head -8
echo "=== guest probe + verdict ==="
LC_ALL=C grep -aE "virtio-pci|virtio_blk|vda|KBOOT588-42|RES-DONE|0x0000|SCRIPT vm0: (PASS|FAIL)|Kernel panic" "$LOG" | head -20
echo "=== on-disk magic seen in guest? ==="
LC_ALL=C grep -a "HYPE588-VBLK-OK" "$LOG" | head -2
LC_ALL=C grep -aq "SCRIPT vm0: PASS" "$LOG" && echo "PASS: kernel VM with a disk reached its shell, no wedge" || { echo "FAIL"; exit 1; }
