#!/bin/sh
# #432: produce the ENROLLED Secure Boot varstore from the empty one the FW_SECBOOT=1 build
# emits. PK/KEK carry a generated "hype" certificate (the platform is hype's, so the platform
# key is too); the Microsoft UEFI CAs (2011 + 2023 families, as shipped by virt-firmware) go
# into KEK/db so stock Windows/shim media verify. Deleting fw/OVMF_VARS.secboot.fd and re-running
# regenerates it -- the PK private key is NOT kept, deliberately: hype never signs anything, and
# an unusable private half cannot leak.
#
# Requires: pip install virt-firmware  (pure python, no root)
set -e
cd "$(dirname "$0")/.."
[ -f fw/OVMF_VARS.secboot-empty.fd ] || {
    echo "enroll-secboot.sh: run FW_SECBOOT=1 tools/build-fw.sh first." >&2; exit 1; }
command -v virt-fw-vars >/dev/null 2>&1 || {
    echo "enroll-secboot.sh: virt-fw-vars not found (pip install virt-firmware)." >&2; exit 1; }
virt-fw-vars --input fw/OVMF_VARS.secboot-empty.fd \
             --output fw/OVMF_VARS.secboot.fd \
             --enroll-generate hype.secureboot --secure-boot
virt-fw-vars --input fw/OVMF_VARS.secboot.fd --print | grep -E "SecureBoot|PK|KEK|db" | head -12
echo "enroll-secboot.sh: fw/OVMF_VARS.secboot.fd ready."
