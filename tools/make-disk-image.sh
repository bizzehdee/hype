#!/bin/sh
# M5-4 (#90): pre-create a RAW virtual-disk image for a `backing=file` guest
# disk, at the size hype.cfg's `size_gb` / `target_disk_size_gb` asks for.
#
# Why a script and not hype itself: hype creates nothing. Post-EBS it has no
# filesystem allocator worth the risk -- #199's file-backed path deliberately
# only ever writes IN PLACE into an already-allocated image (same scoping as
# the ext writer, #204), because growing a file post-ExitBootServices means
# allocating clusters/blocks on a volume the OS may also know about. So the
# image is created here, on a normal OS, before hype ever sees it.
#
# The image is created FULLY ALLOCATED, not sparse. That is deliberate and is
# the whole reason this script exists rather than a bare `truncate`:
#
#   * hype's file-backed writes never grow the file, so a sparse hole is not a
#     "not yet written" region to hype -- it is simply data that reads as
#     zeros and can be written in place. That part is fine.
#   * What is NOT fine is the HOST filesystem needing to allocate a block on
#     first write to a hole. On FAT32/exFAT (#198) hype's writer does not
#     allocate, and on ext (#204) it refuses to; a sparse image would make
#     the guest's first write to each hole fail. Worse, on a nearly-full
#     volume a sparse image can pass creation and then fail mid-install with
#     no space -- exactly the kind of late, confusing failure this project
#     avoids by construction.
#
# Preallocation strategy, best first:
#   fallocate  - real allocation, instant on ext4/xfs/btrfs and on Linux's vfat
#   xfs_io     - same, where fallocate(1) itself is unavailable
#   dd         - portable fallback; writes real zeros, so it is slow but honest
#
# exFAT is DELIBERATELY EXCLUDED from the fast paths (#199 finding, measured):
# exFAT directory entries carry BOTH DataLength and ValidDataLength, and
# fallocate sets only the former. The exFAT driver then returns ZEROS for every
# read past ValidDataLength no matter what is physically in those clusters. The
# guest's writes would be on the medium and invisible to every other exFAT
# reader -- an image nothing but hype could use. Writing the bytes for real is
# the only way to make VDL == DataLength, so on exFAT this always takes the dd
# path. (Measured: fallocate on exFAT left ValidDataLength at 512 for a 1 GiB
# file; a host read at 512 MiB returned zeros while hype's own bytes were
# provably present on disk.)
#
# ext2/ext3/ext4 are excluded from the fast paths for the SAME underlying
# reason, discovered the hard way on real hardware (#696): fallocate() on ext4
# marks the reserved blocks as an UNWRITTEN extent, not a written one. The
# host kernel's ext4 driver hides this from every normal reader by returning
# zeros for reads that land in an unwritten extent -- so probe_tail_valid()
# below, which reads back through that same driver, sees zeros and reports
# the image fine. hype does not go through the host's ext4 driver: its
# resolver walks the on-disk extent tree directly and maps the file to raw
# physical sectors for the hypervisor to read straight off the medium. Under
# that contract an unwritten extent cannot be trusted to read as zero (the
# physical bytes may be leftover data from whatever last used that block), so
# hype's ext resolver correctly REFUSES a file with any unwritten extent
# rather than risk handing a guest garbage. A fallocate-made ext4 image is
# therefore invisible to hype even though every host tool calls it fine. The
# fix is the same as exFAT's: only the dd path leaves every extent genuinely
# written, so ext2/3/4 take it unconditionally too.
#
# Whichever path runs, the result is VERIFIED below rather than trusted --
# including an end-of-file read-back probe that catches exactly this class of
# "allocated but not valid" image on ANY filesystem (though see the ext note
# above: that probe reads through the host driver and cannot itself detect an
# unwritten-extent image -- it is the FAST_OK exclusion that prevents one from
# ever being created in the first place).
# Note also FAT32's 4 GiB-minus-1-byte per-file ceiling, checked separately.
#
# Usage:
#   tools/make-disk-image.sh <path> <size_gb> [--force]
#   tools/make-disk-image.sh --check <path> <size_gb>
#
#   --check   verify an existing image is the right size and fully allocated;
#             create nothing, change nothing. Exit 0 = usable as-is.
#   --force   overwrite an existing file (refused without it -- an existing
#             image is somebody's installed guest).
#
# Examples:
#   tools/make-disk-image.sh /run/media/me/HYPE/hype/disks/win11.img 128
#   tools/make-disk-image.sh --check /mnt/esp/hype/disks/win11.img 128
set -eu

PROG=$(basename "$0")

usage() {
    cat >&2 <<EOF
usage: $PROG <path> <size_gb> [--force]
       $PROG --check <path> <size_gb>

Creates a fully-allocated raw virtual-disk image for a hype \`backing=file\`
disk (hype.cfg size_gb / target_disk_size_gb). Refuses to overwrite an
existing image without --force.
EOF
    exit 2
}

CHECK_ONLY=0
FORCE=0
POSITIONAL=""

for arg in "$@"; do
    case "$arg" in
        --check) CHECK_ONLY=1 ;;
        --force) FORCE=1 ;;
        -h|--help) usage ;;
        -*) echo "$PROG: unknown option $arg" >&2; usage ;;
        *) POSITIONAL="$POSITIONAL $arg" ;;
    esac
done

# shellcheck disable=SC2086
set -- $POSITIONAL
[ $# -eq 2 ] || usage

IMG=$1
SIZE_GB=$2

# size_gb is an integer count of GiB, matching hype.cfg's domain (§5.3). The
# 4096 cap is arbitrary but sane: past that a typo is likelier than intent.
case "$SIZE_GB" in
    ''|*[!0-9]*) echo "$PROG: size_gb must be a positive integer (got '$SIZE_GB')" >&2; exit 2 ;;
esac
[ "$SIZE_GB" -ge 1 ] || { echo "$PROG: size_gb must be >= 1" >&2; exit 2; }
[ "$SIZE_GB" -le 4096 ] || {
    echo "$PROG: size_gb $SIZE_GB looks like a typo (cap 4096); pass a smaller value" >&2
    exit 2
}

WANT_BYTES=$((SIZE_GB * 1024 * 1024 * 1024))

# --- filesystem-imposed limits on the TARGET directory ---------------------
DIR=$(dirname "$IMG")
[ -d "$DIR" ] || { echo "$PROG: directory does not exist: $DIR" >&2; exit 1; }

FSTYPE=$(stat -f -c %T "$DIR" 2>/dev/null || echo unknown)
case "$FSTYPE" in
    msdos)
        # FAT32: 4 GiB - 1 byte per file, hard architectural limit.
        if [ "$WANT_BYTES" -ge 4294967296 ]; then
            echo "$PROG: $DIR is FAT32, whose per-file limit is 4 GiB - 1 byte." >&2
            echo "$PROG: a ${SIZE_GB} GiB image cannot exist there. Use exFAT or ext4," >&2
            echo "$PROG: or a physical-disk target instead of a file-backed one." >&2
            exit 1
        fi
        ;;
esac

# Free space, in bytes, ignoring reserved-for-root slack.
AVAIL_BYTES=$(df -B1 --output=avail "$DIR" 2>/dev/null | tail -1 | tr -d ' ' || echo 0)

report_size() {
    # apparent size, allocated size (both bytes)
    stat -c '%s %b %B' "$1" 2>/dev/null | awk '{print $1" "$2*$3}'
}

# Tail probe: write a magic pattern into the LAST sector, read it back, restore
# zeros. It proves the end of the file is genuinely writable+readable (a sparse
# hole on a full volume fails here).
#
# NOTE, measured: on exFAT this write also makes the driver EXTEND
# ValidDataLength to cover the offset, so the probe REPAIRS a short-VDL image
# rather than reporting it -- it cannot detect that state after the fact,
# because probing fixes it. That is why creation on exFAT does not rely on the
# probe at all and takes the dd path unconditionally: VDL is then correct by
# construction. Running --check on a fallocate-made exFAT image therefore
# silently makes it usable, which is a fine outcome but is a side effect, not a
# diagnosis.
probe_tail_valid() {
    probe_img=$1
    probe_size=$2
    probe_off=$((probe_size - 512))
    printf 'HYPE-IMAGE-TAIL-PROBE' | dd of="$probe_img" bs=1 seek="$probe_off" \
        conv=notrunc status=none
    probe_got=$(dd if="$probe_img" bs=1 skip="$probe_off" count=21 status=none 2>/dev/null || true)
    # restore the sector to zeros either way
    dd if=/dev/zero of="$probe_img" bs=512 seek=$((probe_off / 512)) count=1 \
        conv=notrunc status=none 2>/dev/null || true
    [ "$probe_got" = "HYPE-IMAGE-TAIL-PROBE" ]
}

# --- --check: verify only -------------------------------------------------
if [ "$CHECK_ONLY" -eq 1 ]; then
    [ -f "$IMG" ] || { echo "$PROG: no such image: $IMG" >&2; exit 1; }
    set -- $(report_size "$IMG")
    HAVE_BYTES=$1
    ALLOC_BYTES=$2
    RC=0
    if [ "$HAVE_BYTES" -ne "$WANT_BYTES" ]; then
        echo "$PROG: SIZE MISMATCH: $IMG is $HAVE_BYTES bytes, hype.cfg says ${SIZE_GB} GiB ($WANT_BYTES)" >&2
        RC=1
    fi
    # Allow a little slack: some filesystems round allocation up, and a fully
    # allocated file can report marginally more than its apparent size.
    if [ "$ALLOC_BYTES" -lt "$HAVE_BYTES" ]; then
        echo "$PROG: SPARSE: $IMG has $ALLOC_BYTES of $HAVE_BYTES bytes allocated." >&2
        echo "$PROG: hype never grows a backing file, so writes into the holes will fail." >&2
        echo "$PROG: re-create it with: $PROG '$IMG' $SIZE_GB --force" >&2
        RC=1
    fi
    if [ "$RC" -eq 0 ] && ! probe_tail_valid "$IMG" "$WANT_BYTES"; then
        echo "$PROG: NOT USABLE: a write+read at the end of $IMG did not survive." >&2
        echo "$PROG: the end of the image is not really writable (a hole on a full" >&2
        echo "$PROG: volume, or a read-only mount). Re-create with:" >&2
        echo "$PROG:   $PROG '$IMG' $SIZE_GB --force" >&2
        RC=1
    fi
    if [ "$RC" -eq 0 ]; then
        echo "$PROG: OK: $IMG is ${SIZE_GB} GiB ($WANT_BYTES bytes), fully allocated and valid"
    fi
    exit "$RC"
fi

# --- create ----------------------------------------------------------------
if [ -e "$IMG" ]; then
    [ "$FORCE" -eq 1 ] || {
        echo "$PROG: refusing to overwrite existing image: $IMG" >&2
        echo "$PROG: that file may be an installed guest. Pass --force if you mean it," >&2
        echo "$PROG: or use --check to verify it is already the right shape." >&2
        exit 1
    }
    echo "$PROG: --force given: overwriting $IMG"
fi

if [ "$AVAIL_BYTES" -gt 0 ] && [ "$WANT_BYTES" -gt "$AVAIL_BYTES" ]; then
    echo "$PROG: not enough free space on $DIR: need $WANT_BYTES bytes, $AVAIL_BYTES available" >&2
    exit 1
fi

echo "$PROG: creating $IMG -- ${SIZE_GB} GiB ($WANT_BYTES bytes), fully allocated, on $FSTYPE"

rm -f "$IMG"
FAST_OK=1
case "$FSTYPE" in
    exfat) FAST_OK=0 ;; # ValidDataLength, see the header comment
    ext2|ext3|ext4|ext2/ext3) FAST_OK=0 ;; # unwritten extents, see the header comment (#696)
esac
if [ "$FAST_OK" -eq 1 ] && command -v fallocate > /dev/null 2>&1 &&
   fallocate -l "$WANT_BYTES" "$IMG" 2>/dev/null; then
    METHOD=fallocate
elif [ "$FAST_OK" -eq 1 ] && command -v xfs_io > /dev/null 2>&1 &&
     xfs_io -f -c "falloc 0 $WANT_BYTES" "$IMG" > /dev/null 2>&1; then
    METHOD=xfs_io
else
    # Portable fallback: write real zeros. Slow, but every byte is allocated,
    # which is the property that matters. 1 MiB blocks keep it reasonable.
    if [ "$FAST_OK" -eq 0 ]; then
        echo "$PROG: $FSTYPE needs every byte written (ValidDataLength) -- writing zeros"
    else
        echo "$PROG: no fast preallocation on $FSTYPE -- writing zeros (this will take a while)"
    fi
    rm -f "$IMG"
    dd if=/dev/zero of="$IMG" bs=1048576 count=$((WANT_BYTES / 1048576)) \
       status=progress conv=fsync 2>&1 | tail -1
    METHOD=dd
fi

# Verify what we actually produced rather than trusting the tool: a
# fallocate that silently fell back to sparse would defeat the entire point.
set -- $(report_size "$IMG")
HAVE_BYTES=$1
ALLOC_BYTES=$2
if [ "$HAVE_BYTES" -ne "$WANT_BYTES" ]; then
    echo "$PROG: FAILED: $IMG is $HAVE_BYTES bytes, wanted $WANT_BYTES" >&2
    exit 1
fi
if [ "$ALLOC_BYTES" -lt "$HAVE_BYTES" ]; then
    echo "$PROG: FAILED: $IMG came out SPARSE ($ALLOC_BYTES of $HAVE_BYTES bytes) via $METHOD" >&2
    echo "$PROG: hype cannot grow a backing file, so this image is not usable." >&2
    exit 1
fi

if ! probe_tail_valid "$IMG" "$WANT_BYTES"; then
    echo "$PROG: FAILED: $IMG is allocated but its tail does not read back (via $METHOD)." >&2
    echo "$PROG: the image would read as zeros past some point for other readers." >&2
    exit 1
fi

echo "$PROG: OK: $IMG ${SIZE_GB} GiB, fully allocated and valid via $METHOD"
echo "$PROG: point hype.cfg at it, e.g.:"
echo "$PROG:   target_disk = file:$IMG"
echo "$PROG:   target_disk_size_gb = $SIZE_GB"
