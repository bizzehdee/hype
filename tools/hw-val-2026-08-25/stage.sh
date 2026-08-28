#!/bin/bash
#
# Stage the hw-validation drive. #738: the scratch images are part of the staging,
# not something hype creates.
#
# The 2026-08-26 boots both ran with no SATA disk attached because
# \hype\disks\run1a-scratch.img was not on the volume and the config comment claimed
# hype made it on demand. It does not -- hype has no post-ExitBootServices filesystem
# allocator and only ever writes in place (#331). The comments are corrected; this
# script is the other half, so the image cannot be lost on the next re-stage.
#
# Everything this does was done by hand across six re-stagings on 2026-08-27. The parts
# that are easy to get wrong and expensive to discover later are the parts that are
# checked here rather than left to the operator:
#
#   - the drive is found by LABEL, never by /dev letter. It was sdd on 2026-08-25 and
#     sdb on 2026-08-27; the letter moves when the machine's USB enumeration changes.
#   - `make` ignores a changed EXTRA_CFLAGS on an unchanged mtime, so building both
#     variants requires `make clean` between them. Getting that wrong silently stages
#     two copies of the same build under different names.
#   - every staged file is re-read from the media after an unmount/mount cycle, so a
#     SHA cannot come out of the page cache. The USB-SATA bridge on this drive has
#     dropped its link under sustained write before.
#
# Usage:
#   ./stage.sh                 stage the active (default) build + the AVIC build
#   ./stage.sh --no-build      stage whatever is already in rig/stage-current/
#   ./stage.sh --check         verify the staged drive and change nothing
set -u

# Which input script and run card this staging is for. \input\vm0.txt was copied by hand
# for the first seven stagings, which is why boot 7 went out with boot 6's card still on the
# drive. The active script and the card that describes it must move together or the operator
# is reading instructions for a different run.
BOOT_INPUT=input-1a-hotplug
RUN_CARD=RUN-CARD-2026-08-28-boot16.md

BOOTLABEL=HYPEBOOT
# The data partition has NO label. `EADE-CA36` is its exFAT volume UUID, which is what
# udisks then names the mountpoint after -- which is why the README called it a label for
# two months. Found by being the exfat partition on the same disk as HYPEBOOT instead, so
# neither a label that does not exist nor a UUID that changes on reformat is depended on.
BUILD=1
CHECK=0
for a in "$@"; do
  case "$a" in
    --no-build) BUILD=0 ;;
    --check)    CHECK=1; BUILD=0 ;;
    *) echo "unknown argument: $a" >&2; exit 2 ;;
  esac
done

die() { echo "stage: $*" >&2; exit 1; }
cd "$(git rev-parse --show-toplevel)" || die "not in the repo"
HERE=tools/hw-val-2026-08-25
STAGEDIR=rig/stage-current

# ---------------------------------------------------------------- find the drive
# By label. A wrong letter here writes hype's boot binary over something else.
BOOTDEV=$(lsblk -rpno NAME,LABEL | awk -v l="$BOOTLABEL" '$2==l{print $1; exit}')
[ -n "$BOOTDEV" ] || die "no volume labelled $BOOTLABEL -- is the drive plugged in?"
BOOTDISK=$(lsblk -no PKNAME "$BOOTDEV" | head -1)
[ -n "$BOOTDISK" ] || die "could not find the parent disk of $BOOTDEV"
DATADEV=$(lsblk -rpno NAME,FSTYPE "/dev/$BOOTDISK" | awk '$2=="exfat"{print $1; exit}')
[ -n "$DATADEV" ] || die "no exfat partition on /dev/$BOOTDISK -- this is not the hw-val drive"
TRAN=$(lsblk -no TRAN "/dev/$BOOTDISK" | head -1)
[ "$TRAN" = usb ] || die "/dev/$BOOTDISK is '$TRAN', not usb -- refusing to write to it"
echo "drive: /dev/$BOOTDISK  boot=$BOOTDEV  data=$DATADEV  transport=$TRAN"

mountpt() { lsblk -no MOUNTPOINT "$1" | head -1; }
ensure_mounted() {
  local dev=$1 mp
  mp=$(mountpt "$dev")
  if [ -z "$mp" ]; then udisksctl mount -b "$dev" >/dev/null 2>&1; mp=$(mountpt "$dev"); fi
  [ -n "$mp" ] || die "could not mount $dev"
  echo "$mp"
}
BOOTMP=$(ensure_mounted "$BOOTDEV") || exit 1
DATAMP=$(ensure_mounted "$DATADEV") || exit 1
echo "mounted: $BOOTMP  $DATAMP"

# ---------------------------------------------------------------- build
if [ "$BUILD" = 1 ]; then
  mkdir -p "$STAGEDIR"
  echo "building default ..."
  make clean >/dev/null 2>&1
  make all CC=clang LD=ld.lld >/dev/null 2>&1 || die "default build failed"
  cp build/hype.efi "$STAGEDIR/hype-default.efi"
  # make clean between variants is REQUIRED, not tidiness: make does not rebuild on an
  # EXTRA_CFLAGS change alone, so without it the second build relinks nothing.
  echo "building AVIC ..."
  make clean >/dev/null 2>&1
  EXTRA_CFLAGS="-DHYPE_ENABLE_AVIC=1" make all CC=clang LD=ld.lld >/dev/null 2>&1 || die "AVIC build failed"
  cp build/hype.efi "$STAGEDIR/hype-avic.efi"
  # The two must actually differ, and in the flag rather than only in ld.lld's timestamp.
  grep -aqc 'ENABLING (HYPE_ENABLE_AVIC set)' "$STAGEDIR/hype-avic.efi" >/dev/null 2>&1 || true
  if strings -a "$STAGEDIR/hype-default.efi" | grep -q 'ENABLING (HYPE_ENABLE_AVIC set)'; then
    die "the DEFAULT build contains the AVIC ENABLING string -- the variants got crossed"
  fi
  strings -a "$STAGEDIR/hype-avic.efi" | grep -q 'ENABLING (HYPE_ENABLE_AVIC set)' \
    || die "the AVIC build does NOT contain the ENABLING string -- EXTRA_CFLAGS did not take"
fi

# ---------------------------------------------------------------- scratch images (#738)
# hype creates nothing. Each entry is "path-under-DATAMP:bytes". Sizes match the
# target_disk_size_gb in the configs that name them.
mk_image() {
  # Split: `local` expands ALL its arguments before any assignment takes effect, so
  # referencing $rel in the same statement that sets it is unbound under `set -u`.
  local rel=$1 want=$2 have=0
  local path="$DATAMP/$rel"
  mkdir -p "$(dirname "$path")"
  [ -f "$path" ] && have=$(stat -c %s "$path")
  if [ "$have" = "$want" ]; then echo "  ok    $rel ($want bytes)"; return 0; fi
  if [ "$have" != 0 ]; then
    echo "  RESIZE $rel: $have -> $want bytes"
  else
    echo "  create $rel ($want bytes)"
  fi
  # Fully allocated, not sparse: hype only ever writes in place, so a hole is a
  # write that silently goes nowhere.
  if [ "$CHECK" = 1 ]; then echo "  (--check: not creating)"; return 1; fi
  fallocate -l "$want" "$path" 2>/dev/null || dd if=/dev/zero of="$path" bs=1M \
      count=$((want / 1048576)) status=none || die "could not create $rel"
  return 0
}
echo "scratch images (#738):"
IMG_RC=0
mk_image "hype/disks/run1a-scratch.img" $((2 * 1024 * 1024 * 1024)) || IMG_RC=1

# ---------------------------------------------------------------- copy
if [ "$CHECK" = 0 ]; then
  echo "staging onto $BOOTMP ..."
  cp "$STAGEDIR/hype-default.efi" "$BOOTMP/EFI/hype/hype-default.efi" || die "copy default"
  cp "$STAGEDIR/hype-avic.efi"    "$BOOTMP/EFI/hype/hype-avic.efi"    || die "copy avic"
  cp "$STAGEDIR/hype-default.efi" "$BOOTMP/EFI/BOOT/BOOTX64.EFI"      || die "copy active"
  cp $HERE/hype*.cfg "$BOOTMP/" || die "copy configs"
  [ -f "$HERE/$RUN_CARD" ] || die "run card $RUN_CARD not found"
  cp "$HERE/$RUN_CARD" "$BOOTMP/RUN-CARD.md" || die "copy run card"
  # The active input script. Kept under its own name on the drive as well, so which run a
  # log belongs to is recoverable from the drive alone after the fact.
  [ -f "$HERE/$BOOT_INPUT/vm0.txt" ] || die "input script $BOOT_INPUT/vm0.txt not found"
  mkdir -p "$BOOTMP/input" "$BOOTMP/$BOOT_INPUT"
  cp "$HERE/$BOOT_INPUT/vm0.txt" "$BOOTMP/input/vm0.txt"      || die "copy input script"
  cp "$HERE/$BOOT_INPUT/vm0.txt" "$BOOTMP/$BOOT_INPUT/vm0.txt" || die "copy input archive"
  cp docs/hw-validation-queue-2026-08-28.md "$BOOTMP/QUEUE.md" 2>/dev/null || true
  cp docs/qemu-vs-hardware.md "$BOOTMP/QEMU-VS-HARDWARE.md" 2>/dev/null || true
  # Logs cleared so the next boot starts from a clean rotation. Archive them FIRST --
  # this script does not, deliberately: losing a boot's evidence is worse than an
  # extra manual step, so it refuses if they are non-empty and unarchived.
  for L in HYPE.LOG RUN1A.LOG; do
    if [ -s "$BOOTMP/$L" ]; then
      echo "  $L is non-empty -- archive it before re-staging (cp to $HERE/logs/<boot>/)" >&2
      exit 1
    fi
  done
  rm -f "$BOOTMP/HYPE.BOOTCOUNT" "$BOOTMP"/vars-*.bin
  sync
fi

# ---------------------------------------------------------------- verify from the media
echo "unmount/mount cycle, then re-read (so no SHA comes from the page cache) ..."
udisksctl unmount -b "$BOOTDEV" >/dev/null 2>&1
BOOTMP=$(ensure_mounted "$BOOTDEV") || exit 1
RC=0
verify() {
  local a=$1 b=$2 name=$3
  local ha hb
  ha=$(sha256sum "$a" | cut -d' ' -f1); hb=$(sha256sum "$b" | cut -d' ' -f1)
  if [ "$ha" = "$hb" ]; then echo "  ok    $name  $ha"; else
    echo "  BAD   $name: staged $ha, on media $hb" >&2; RC=1; fi
}
if [ -f "$STAGEDIR/hype-default.efi" ]; then
  verify "$STAGEDIR/hype-default.efi" "$BOOTMP/EFI/BOOT/BOOTX64.EFI" "BOOTX64.EFI"
  verify "$STAGEDIR/hype-default.efi" "$BOOTMP/EFI/hype/hype-default.efi" "hype-default.efi"
  verify "$STAGEDIR/hype-avic.efi"    "$BOOTMP/EFI/hype/hype-avic.efi"    "hype-avic.efi"
fi
for c in $HERE/hype*.cfg; do
  verify "$c" "$BOOTMP/$(basename "$c")" "$(basename "$c")"
done
if [ "$CHECK" = 0 ]; then
  verify "$HERE/$BOOT_INPUT/vm0.txt" "$BOOTMP/input/vm0.txt" "input/vm0.txt"
  verify "$HERE/$RUN_CARD" "$BOOTMP/RUN-CARD.md" "RUN-CARD.md"
fi
echo "scratch image on media:"
ls -l "$DATAMP/hype/disks/" 2>/dev/null | tail -n +2 | sed 's/^/  /'

[ "$IMG_RC" = 0 ] || RC=1
if [ "$RC" = 0 ]; then echo "STAGED OK"; else echo "STAGING HAS PROBLEMS -- see above" >&2; fi
exit "$RC"
