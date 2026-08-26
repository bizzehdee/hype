#!/bin/bash
# #433: boot a Linux guest with tpm = on and let its OWN tpm_crb driver judge -- the chip binds,
# reports TPM 2.0, and /sys/class/tpm/tpm0 exists. PASS = the in-guest version reads 2.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-$(mktemp -d /mnt/data/dev/hype/disk-images/rig433.XXXXXX)}"
echo "scratch: $S"
killall -9 "$(basename "$QEMU")" 2>/dev/null || true
sleep 1
HYPE_CFG=tools/433/hype.cfg HYPE_INPUT=tools/433/tpm-vm0.txt \
    tools/run-guest.sh disk-images/alpine-hype-dbg.iso 433-tpm "${1:-300}" || true
LOG=disk-images/run-433-tpm.log
echo "=== the guest's TPM ==="
LC_ALL=C grep -a "TPM2\|tpm_crb\|TVER\|TPM433\|SCRIPT vm0: PASS\|SCRIPT vm0: FAIL" "$LOG" | head -8
LC_ALL=C grep -aq "SCRIPT vm0: PASS" "$LOG" || { echo "FAIL"; exit 1; }
echo "PASS: guest tpm_crb bound a TPM 2.0"
