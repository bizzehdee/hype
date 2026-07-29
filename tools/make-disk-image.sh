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
#   fallocate  - real allocation, instant on ext4/xfs/btrfs, and it also works
#                on Linux's vfat/exfat drivers (measured: a 1 GiB image on a
#                real FAT32 volume came out fully allocated via fallocate)
#   xfs_io     - same, where fallocate(1) itself is unavailable
#   dd         - portable fallback; writes real zeros, so it is slow but honest
# Whichever path runs, the result is VERIFIED below rather than trusted. Note
# FAT32's 4 GiB-minus-1-byte per-file ceiling, checked separately.
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
    if [ "$RC" -eq 0 ]; then
        echo "$PROG: OK: $IMG is ${SIZE_GB} GiB ($WANT_BYTES bytes), fully allocated"
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
if command -v fallocate > /dev/null 2>&1 && fallocate -l "$WANT_BYTES" "$IMG" 2>/dev/null; then
    METHOD=fallocate
elif command -v xfs_io > /dev/null 2>&1 &&
     xfs_io -f -c "falloc 0 $WANT_BYTES" "$IMG" > /dev/null 2>&1; then
    METHOD=xfs_io
else
    # Portable fallback: write real zeros. Slow, but every byte is allocated,
    # which is the property that matters. 1 MiB blocks keep it reasonable.
    echo "$PROG: no fast preallocation on $FSTYPE -- writing zeros (this will take a while)"
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

echo "$PROG: OK: $IMG ${SIZE_GB} GiB, fully allocated via $METHOD"
echo "$PROG: point hype.cfg at it, e.g.:"
echo "$PROG:   target_disk = file:$IMG"
echo "$PROG:   target_disk_size_gb = $SIZE_GB"
