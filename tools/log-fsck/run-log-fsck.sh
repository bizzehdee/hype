#!/bin/sh
# #597/#596: drive hype's ACTUAL log-writing path (core/log_sink.c + logbuf + log_split) at a real
# mkfs.vfat volume and judge it independently. See log_fsck.c for what each scenario reproduces.
#
# Tests (>= 3 as requested), each a scenario x barrier policy:
#   combined/ok   -- \HYPE.LOG alone, the budgeted ordered-prefix flush cadence.
#   split/ok      -- \HYPE.LOG + 3 per-VM logs on ONE shared fs, chains growing interleaved.
#   split/flaky   -- the same, but the barrier is INTERMITTENTLY rejected (#516): writes mostly
#                    succeed, occasional rollbacks happen while other files grow on the shared fs.
#   longrun/ok    -- split + the #585 reclaim step over a long run (VM1.LOG came from a long run).
#   longrun/flaky -- the long run with intermittent barrier rejection.
#
# Verdict per test:
#   - fsck.vfat -n must be STRUCTURALLY clean (a dirty bit is allowed, not damage);
#   - every log file mtools can read back, at the exact length hype's own reader reports -- a file
#     hype lost (size -1), one mtools cannot read (a chain Linux rejects = the #596 signal), or a
#     length disagreement, all fail.
set -eu
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
OUT=${OUT:-"$ROOT/disk-images/log-fsck"}
SIZE=${SIZE:-4G}
SPC=${SPC:-64}   # 32 KiB clusters -- the stick geometry

for t in mkfs.vfat fsck.vfat mcopy; do
    command -v "$t" >/dev/null 2>&1 || { echo "MISSING TOOL: $t (need dosfstools + mtools)"; exit 2; }
done
mkdir -p "$OUT"

# fs_ops's registry drags the whole core tree; exclude the networking/USB/halt modules that pull
# device symbols with no host build, and stub the two symbols the FAT/log path might still reach.
libs=""
for f in "$ROOT"/core/*.c; do
    case "$f" in
        *_hw.c|*/blk_usb.c|*/xhci.c|*/usb_msc.c|*/usb_hid.c|*/e1000.c|*/e1000_dev_ring.c|\
        */virtio_net_ring.c|*/arp.c|*/nat.c|*/guest_nic.c|*/l2switch.c|*/halt.c) continue ;;
    esac
    libs="$libs $f"
done
cat > "$OUT/stubs.c" <<'EOF'
#include <stdio.h>
void hype_fatal(const char *msg){ fprintf(stderr, "hype_fatal: %s\n", msg?msg:""); }
void hype_serial_putc(char c){ (void)c; }
EOF
cc -O1 -g -fno-builtin -I"$ROOT" -o "$OUT/log_fsck" log_fsck.c "$OUT/stubs.c" $libs

overall=0
run_case() {
    scenario="$1"; policy="$2"
    tag="$scenario-$policy"
    img="$OUT/$tag.img"; man="$OUT/$tag.manifest"
    rm -f "$img" "$man"
    truncate -s "$SIZE" "$img"
    mkfs.vfat -F 32 -s "$SPC" -n HYPELOG "$img" >/dev/null

    echo "=== $tag ==="
    drv=0
    "$OUT/log_fsck" "$img" "$scenario" "$policy" "$man" | sed 's/^/  /' || drv=$?
    # PIPESTATUS isn't portable to /bin/sh; re-check by presence of a manifest with a HYPE.LOG line
    if ! grep -q '^HYPE.LOG ' "$man" 2>/dev/null; then
        echo "  DRIVER ABORTED before writing a manifest"; overall=1; return
    fi

    rc=0
    # structural
    fsck.vfat -n "$img" > "$OUT/fsck-$tag.txt" 2>&1 || true
    if grep -qiE "cluster chain|beyond EOF|Free cluster summary wrong|orphan|lost cluster|Bad cluster|allocation size|Contains a free cluster" "$OUT/fsck-$tag.txt"; then
        echo "  fsck.vfat: STRUCTURAL DAMAGE"; grep -vE '^$' "$OUT/fsck-$tag.txt" | sed 's/^/    /'; rc=1
    elif grep -qi "Dirty bit is set" "$OUT/fsck-$tag.txt"; then
        echo "  fsck.vfat: clean (dirty bit set -- expected under a failed barrier)"
    else
        echo "  fsck.vfat: CLEAN"
    fi
    # independent read-back at hype's reported length
    while read -r name hsize; do
        [ -n "$name" ] || continue
        if [ "$hsize" = "-1" ]; then
            echo "  LOST: hype itself cannot reopen $name"; rc=1; continue
        fi
        out="$OUT/mc.tmp"; rm -f "$out"
        if ! mcopy -n -i "$img" "::$name" "$out" 2>/dev/null; then
            echo "  UNREADABLE: mtools cannot read $name (Linux would EIO -- the #596 signal)"; rc=1; continue
        fi
        actual=$(stat -c%s "$out")
        if [ "$actual" != "$hsize" ]; then
            echo "  LENGTH DISAGREE: $name hype=$hsize mtools=$actual"; rc=1
        else
            echo "  ok: $name $hsize bytes readable + length agrees"
        fi
    done < "$man"
    [ "$rc" -eq 0 ] || overall=1
}

run_case combined ok
run_case split ok
run_case split flaky
run_case longrun ok
run_case longrun flaky

rm -f "$OUT/mc.tmp"
if [ "$overall" -eq 0 ]; then
    echo "PASS: hype's log-writing path produced clean, readable volumes across all scenarios"
else
    echo "FAIL: a log-writing scenario left an inconsistent or unreadable volume -- see above"
fi
exit "$overall"
