#!/bin/sh
# #329: two small raw disk images for the multi-disk QEMU rig, each tagged so a mix-up is VISIBLE.
#
# The tag is written at sector 0 AND at the last sector: sector 0 alone would not catch a backend
# that resolved the right file with the wrong capacity, which is the failure the zero-sector CHS
# trap in fw_1_attach_storage already showed is easy to hit.
set -eu
out="${1:-build}"
mkdir -p "$out"
python3 - "$out" <<'PY'
import sys
out = sys.argv[1]
for name, tag in (("diska.img", b"HYPE-DISK-A-SLOT0"), ("diskb.img", b"HYPE-DISK-B-SLOT1")):
    b = bytearray(8 * 1024 * 1024)
    b[0:len(tag)] = tag
    b[510], b[511] = 0x55, 0xAA          # a plausible MBR signature, so firmware probes it
    b[-512:-512 + len(tag)] = tag
    open(f"{out}/{name}", "wb").write(bytes(b))
    print(f"{out}/{name}: {len(b)} bytes, tag {tag.decode()}")
PY
