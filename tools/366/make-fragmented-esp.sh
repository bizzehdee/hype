#!/bin/bash
#
# #366: build a GPT+FAT32 ESP whose \iso\test.iso is deliberately fragmented into MORE than 64
# extents, so hype's streaming cap can be exercised on purpose rather than waited for.
#
#   tools/366/make-fragmented-esp.sh <iso> <out.img> [pad-MiB]
#
# Whether an ISO streams used to depend on how the operator's stick happened to be laid out. That
# is not something a test can wait to encounter, so this manufactures the layout.
#
# The method, from the FAT half of the ext4/FAT test-volume recipe. Writing filler and deleting
# some of it does NOT work -- the allocator just uses the large free tail and the file lands in one
# extent. What works is filling the volume almost completely and then freeing ALTERNATING pieces,
# so no single free run is bigger than one pad and the file must be split across all of them.
#
# Extent count is therefore roughly (ISO size / pad size). The default 2 MiB pad puts a 266 MB ISO
# at ~133 extents: comfortably past the old 64 cap and inside the current 256 one, which is exactly
# the band that distinguishes them.
#
# No root: sfdisk + mtools only, same constraint tools/262 and tools/258 work under.
set -e
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"

ISO="$1"; OUT="$2"; PAD_MB="${3:-2}"
[ -n "$ISO" ] && [ -f "$ISO" ] || { echo "usage: $0 <iso> <out.img> [pad-MiB]"; exit 1; }
[ -n "$OUT" ] || { echo "usage: $0 <iso> <out.img> [pad-MiB]"; exit 1; }
[ -f build/hype.efi ] || { echo "build/hype.efi missing -- run make all"; exit 1; }

ISO_MB=$(( $(stat -c%s "$ISO") / 1048576 + 1 ))
# Firmware + slack, then room for the pads. The volume must be only a little larger than what it
# will hold: a generous tail is a single big free run, which defeats the whole exercise.
FS_MB=$(( ISO_MB * 2 + 32 ))
IMG_MB=$(( FS_MB + 2 ))

rm -f "$OUT"
fallocate -l "${IMG_MB}M" "$OUT" 2>/dev/null || \
    dd if=/dev/zero of="$OUT" bs=1048576 count="$IMG_MB" status=none conv=fsync
# GPT is not optional: hype locates the volume with hype_gpt_find_partition() before handing it to
# core/fat.c, so a bare FAT image has no partition for it to find.
sfdisk --label gpt -q "$OUT" >/dev/null <<SFDISK
2048,,U
SFDISK

mformat -i "$OUT@@1M" -F ::
mmd -i "$OUT@@1M" ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso
mcopy -i "$OUT@@1M" build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$OUT@@1M" fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/

PAD=$(mktemp)
trap 'rm -f "$PAD"' EXIT
dd if=/dev/zero of="$PAD" bs=1048576 count="$PAD_MB" status=none

# Fill until the volume refuses another pad, so the free space left is only what we free below.
n=0
while mcopy -i "$OUT@@1M" "$PAD" "::/p$n.bin" 2>/dev/null; do
    n=$((n + 1))
done
echo "filled with $n x ${PAD_MB} MiB pads"

# Free every OTHER pad. Each hole is exactly one pad, so the ISO cannot find a run longer than that.
for i in $(seq 0 2 $((n - 1))); do
    mdel -i "$OUT@@1M" "::/p$i.bin" 2>/dev/null || true
done

mcopy -i "$OUT@@1M" "$ISO" ::/iso/test.iso
sync "$OUT"

# Verify what was produced rather than trusting the tools -- a short write here looks exactly like
# hype failing to boot.
want=$(stat -c%s "$ISO")
have=$(mdir -i "$OUT@@1M" ::/iso 2>/dev/null | awk '/test *iso/ {print $3}')
[ "$have" = "$want" ] || { echo "verify FAILED: test.iso is $have bytes, wanted $want"; exit 1; }
mdir -i "$OUT@@1M" ::/EFI/BOOT 2>/dev/null | grep -q BOOTX64 || \
    { echo "verify FAILED: BOOTX64.EFI missing"; exit 1; }
echo "built $OUT: fragmented \\iso\\test.iso, ${PAD_MB} MiB holes"
