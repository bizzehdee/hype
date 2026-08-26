#!/bin/bash
# #545 leg: real bzImage + initramfs direct boot. PASS = the in-guest computed marker.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
cd "$(git rev-parse --show-toplevel)"
killall -9 "$(basename "$QEMU")" 2>/dev/null || true
sleep 1
HYPE_CFG=tools/545/hype.cfg \
HYPE_KERNELS="disk-images/545/vmlinuz-virt disk-images/545/initramfs-virt" \
HYPE_INPUT=tools/545/shell-vm0.txt \
    tools/run-guest.sh disk-images/alpine-hype-dbg.iso 545-kernel "${TIMEOUT:-300}" || true
LOG=disk-images/run-545-kernel.log
echo "=== load + verdict ==="
LC_ALL=C grep -a "kernel '\\\\EFI\|initrd\|SCRIPT vm0: PASS\|SCRIPT vm0: FAIL\|Kernel panic" "$LOG" | head -8
LC_ALL=C grep -aq "SCRIPT vm0: PASS" "$LOG" && echo "PASS: real kernel to a shell" || { echo "FAIL"; exit 1; }
