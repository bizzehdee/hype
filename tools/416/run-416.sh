#!/bin/sh
# #416: prove the NTFS dirty-flag bracket and fixup re-stamp against a real mkntfs/ntfs-3g volume.
#
# hype does not implement $LogFile replay (plan.md §10 decision 64 -- ntfs-3g, this project's own
# reference, doesn't either). What this proves instead: hype's dirty-flag bracket (txn_open/close)
# is read the same way by a real NTFS driver as Windows's own dirty bit, that hype's own read-only
# mount (#337) correctly refuses a volume made dirty this way, and that re-stamping a record's
# fixups through hype_ntfs_record_write (including the $MFTMirr mirror this ticket's own real-image
# run discovered was needed) leaves the volume exactly as valid to ntfs-3g as before.
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
OUT=${OUT:-/tmp/hype-416}
mkdir -p "$OUT"

cc -O1 -g -I"$ROOT" -o "$OUT/journal_repro" journal_repro.c \
    "$ROOT/core/ntfs.c" "$ROOT/core/ntfs_journal.c" "$ROOT/core/fs_owner_guard.c" \
    "$ROOT/core/file_range.c"

fresh_volume() {
    rm -f "$1"
    dd if=/dev/zero of="$1" bs=1M count=64 status=none
    mkntfs -F -Q -L HYPE416 "$1" >/dev/null 2>&1
}

rc=0

echo "--- fresh mkntfs volume ---"
img="$OUT/ntfs416-a.img"
fresh_volume "$img"
"$OUT/journal_repro" "$img" mount
if ! ntfsinfo -m "$img" 2>/dev/null | grep -q "Volume Flags: 0x0000"; then
    echo "FAIL: fresh mkntfs volume is not clean per ntfsinfo"
    rc=1
else
    echo "ntfsinfo confirms: Volume Flags: 0x0000 (clean)"
fi

echo "--- hype_ntfs_txn_open: set dirty, then confirm hype's OWN read-only mount refuses it ---"
"$OUT/journal_repro" "$img" set-dirty
if "$OUT/journal_repro" "$img" mount | grep -q "^dirty="; then
    echo "FAIL: hype_ntfs_mount unexpectedly succeeded on a dirty volume (#337's gate broke)"
    rc=1
else
    echo "hype_ntfs_mount correctly refuses the now-dirty volume (#337's gate intact)"
fi
if ! ntfsinfo -m "$img" 2>&1 | grep -qi "scheduled for check"; then
    echo "FAIL: ntfsinfo does not refuse the volume as dirty after txn_open"
    rc=1
else
    echo "ntfsinfo confirms: volume is scheduled for check (dirty) -- refuses to mount, like hype's own #337 gate"
fi

echo "--- hype_ntfs_txn_open + txn_close bracket, one mounted session ---"
img2="$OUT/ntfs416-b.img"
fresh_volume "$img2"
"$OUT/journal_repro" "$img2" bracket > "$OUT/bracket.log" 2>&1 &
bracket_pid=$!
# give the child time to print DIRTY-SET and enter its sleep window
sleep 1
if ! grep -q "^DIRTY-SET$" "$OUT/bracket.log"; then
    echo "FAIL: bracket process did not report DIRTY-SET in time"
    rc=1
fi
if ! ntfsinfo -m "$img2" 2>&1 | grep -qi "scheduled for check"; then
    echo "FAIL: ntfsinfo does not refuse the volume as dirty mid-bracket"
    rc=1
else
    echo "ntfsinfo confirms: volume is scheduled for check (dirty), mid-bracket before close"
fi
wait "$bracket_pid"
if ! grep -q "^OK$" "$OUT/bracket.log"; then
    echo "FAIL: bracket process did not report OK"
    cat "$OUT/bracket.log"
    rc=1
fi
if ntfsinfo -m "$img2" 2>&1 | grep -qi "scheduled for check"; then
    echo "FAIL: ntfsinfo still refuses the volume as dirty after txn_close"
    rc=1
else
    echo "ntfsinfo mounts cleanly again after txn_close"
fi

echo "--- hype_ntfs_record_write: fixup re-stamp + \$MFTMirr round trip ---"
"$OUT/journal_repro" "$img2" restamp
ntfsfix -n "$img2" > "$OUT/fsck-restamp.txt" 2>&1 || true
if grep -qiE "does not match|error|corrupt|fixup" "$OUT/fsck-restamp.txt"; then
    echo "FAIL: ntfsfix reports a problem after the fixup re-stamp"
    sed 's/^/    /' "$OUT/fsck-restamp.txt"
    rc=1
else
    echo "ntfsfix -n: clean after the fixup re-stamp (\$MFTMirr agrees with \$MFT)"
fi
"$OUT/journal_repro" "$img2" mount

if [ "$rc" -eq 0 ]; then
    echo "PASS: dirty-flag bracket and fixup re-stamp both verified against real ntfs-3g tooling"
else
    echo "FAIL: see above"
fi
exit "$rc"
