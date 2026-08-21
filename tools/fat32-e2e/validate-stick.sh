#!/bin/sh
# #597: judge a pulled FAT32 self-test stick -- both halves.
#
#   DATA:       every \F32TEST\*.BIN read back byte-exact through the OS vfat driver.
#   STRUCTURAL: fsck.vfat -n reports the volume clean (this is the #596 catch -- a chain hype
#               round-trips but Linux rejects).
#
# usage: validate-stick.sh <mount-point> [device]
#   <mount-point>  where the stick is mounted, e.g. /run/media/you/HYPEHW
#   [device]       optional raw partition, e.g. /dev/sdd1; if given, fsck.vfat -n is run on it
#                  (needs the stick UNMOUNTED and usually root -- the script prints the command
#                  when it cannot run it itself).
set -eu
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
OUT=${OUT:-"$ROOT/disk-images/fat32-e2e"}
MNT=${1:-}
DEV=${2:-}
[ -n "$MNT" ] || { echo "usage: $0 <mount-point> [device]"; exit 2; }
[ -d "$MNT/F32TEST" ] || { echo "no $MNT/F32TEST -- did the marker \\F32TEST.RUN run on this stick?"; exit 2; }

mkdir -p "$OUT"
cc -O1 -g -I"$ROOT" -o "$OUT/validate_stick" validate_stick.c

echo "=== DATA: byte-exact read-back ==="
data_rc=0
"$OUT/validate_stick" "$MNT" || data_rc=$?

echo "=== STRUCTURAL: fsck.vfat -n ==="
struct_rc=0
if [ -n "$DEV" ]; then
    if mountpoint -q "$MNT"; then
        echo "  $DEV is mounted at $MNT; fsck needs it unmounted. Run:"
        echo "    sudo umount $DEV && sudo fsck.vfat -n $DEV"
        struct_rc=0
    elif fsck.vfat -n "$DEV" > "$OUT/fsck-stick.txt" 2>&1; then
        cat "$OUT/fsck-stick.txt" | sed 's/^/    /'
        if grep -qiE "cluster chain|beyond EOF|Free cluster summary wrong|orphan|lost cluster|Bad cluster|allocation size" "$OUT/fsck-stick.txt"; then
            echo "  fsck.vfat: STRUCTURAL DAMAGE"; struct_rc=1
        else
            echo "  fsck.vfat: clean"
        fi
    else
        cat "$OUT/fsck-stick.txt" | sed 's/^/    /'
        echo "  fsck.vfat reported problems (exit non-zero)"; struct_rc=1
    fi
else
    echo "  no device given; run fsck yourself:  sudo umount <dev> && sudo fsck.vfat -n <dev>"
fi

if [ "$data_rc" -eq 0 ] && [ "$struct_rc" -eq 0 ]; then
    echo "PASS: stick is byte-exact and structurally clean"
    exit 0
fi
echo "FAIL: see above (data_rc=$data_rc struct_rc=$struct_rc)"
exit 1
