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
#   ./stage.sh                 stage the default, AVIC and APICv builds; boot set amd1
#   ./stage.sh --boot intelb   the same, for the Intel APICv boot (APICv build active)
#   ./stage.sh --boot amdl0    the AMD LAPTOP #799 boot: hype1a.cfg, default build, 5 minutes
#   ./stage.sh --no-build      stage whatever is already in rig/stage-current/
#   ./stage.sh --check         verify the staged drive and change nothing
set -u

# Which input script and run card this staging is for. \input\vm0.txt was copied by hand
# for the first seven stagings, which is why boot 7 went out with boot 6's card still on the
# drive. The active script and the card that describes it must move together or the operator
# is reading instructions for a different run.
# `--boot <name>` selects the set. Each set names the input script, the run card, the config
# that becomes \hype.cfg (the others are staged beside it as fallbacks) and which build
# variant becomes \EFI\BOOT\BOOTX64.EFI. All three variants are always staged under
# \EFI\hype\ so a different one can be made active by copying, without a rebuild.
BOOT=amd1
NEED_DATA_FS=0
select_boot() {
  case "$1" in
    amd1)   BOOT_INPUT=input-1a; RUN_CARD=RUN-CARD-2026-09-03-bootAMD1.md
            ACTIVE_CFG=hype1g.cfg; ACTIVE_BUILD=default ;;
    # Boot 1 of docs/hw-validation-queue-2026-09-09.md: the APICv run plus the ext4/NTFS
    # data-partition legs (#688 #689) and the scratch-stick write + pull (#388 #754). Needs
    # the four-partition drive layout (FAT32 + exFAT + ext4 + NTFS); refuses without it.
    intelb) BOOT_INPUT=input-2h; RUN_CARD=RUN-CARD-2026-09-09-boot1-intel.md
            ACTIVE_CFG=hype2h.cfg; ACTIVE_BUILD=apicv; NEED_DATA_FS=1 ;;
    # L0 on the AMD LAPTOP, not the 5950X: five minutes to read one HOUSECOST line for #799.
    # Deliberately the same config and input script as the 2026-09-03 laptop attempt that
    # measured 235 us/exit, so the two runs are comparable. The laptop is serial-less and
    # cold-boot only, so HYPE.LOG on the drive is the whole record.
    amdl0)  BOOT_INPUT=input-1a; RUN_CARD=RUN-CARD-2026-09-03-bootAMDL0.md
            ACTIVE_CFG=hype1a.cfg; ACTIVE_BUILD=default ;;
    *) echo "unknown boot set: $1 (amd1|intelb|amdl0)" >&2; exit 2 ;;
  esac
}

BOOTLABEL=HYPEBOOT
# The data partition has NO label. `EADE-CA36` is its exFAT volume UUID, which is what
# udisks then names the mountpoint after -- which is why the README called it a label for
# two months. Found by being the exfat partition on the same disk as HYPEBOOT instead, so
# neither a label that does not exist nor a UUID that changes on reformat is depended on.
BUILD=1
CHECK=0
while [ $# -gt 0 ]; do
  case "$1" in
    --no-build) BUILD=0 ;;
    --check)    CHECK=1; BUILD=0 ;;
    --boot)     shift; BOOT=${1:-}; [ -n "$BOOT" ] || { echo "--boot needs a name" >&2; exit 2; } ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done
select_boot "$BOOT"
echo "boot set: $BOOT  input=$BOOT_INPUT cfg=$ACTIVE_CFG active-build=$ACTIVE_BUILD card=$RUN_CARD"

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
# 2026-09-09 layout: ext4 and NTFS data partitions beside the exFAT one (#688 #689). hype's
# path resolver walks GPT partitions 1..4 and takes the FIRST volume holding the path, so a
# file meant to be served from ext4 or NTFS must not also exist on the exFAT partition.
EXT4DEV=$(lsblk -rpno NAME,FSTYPE "/dev/$BOOTDISK" | awk '$2=="ext4"{print $1; exit}')
NTFSDEV=$(lsblk -rpno NAME,FSTYPE "/dev/$BOOTDISK" | awk '$2=="ntfs"{print $1; exit}')
EXT4MP=""; NTFSMP=""
if [ -n "$EXT4DEV" ] && [ -n "$NTFSDEV" ]; then
  EXT4MP=$(ensure_mounted "$EXT4DEV") || exit 1
  NTFSMP=$(ensure_mounted "$NTFSDEV") || exit 1
  echo "data filesystems: ext4=$EXT4MP  ntfs=$NTFSMP"
elif [ "$NEED_DATA_FS" = 1 ]; then
  die "boot set $BOOT needs ext4 and NTFS partitions on /dev/$BOOTDISK -- rebuild the drive first (docs/hw-validation-queue-2026-09-09.md)"
fi

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
  echo "building APICv ..."
  make clean >/dev/null 2>&1
  EXTRA_CFLAGS="-DHYPE_ENABLE_APICV=1" make all CC=clang LD=ld.lld >/dev/null 2>&1 || die "APICv build failed"
  cp build/hype.efi "$STAGEDIR/hype-apicv.efi"
  # The microtest kernels ride along: hype2d/hype2g run them, and a stale .bin proves nothing
  # about the hype it is staged with.
  make micro >/dev/null 2>&1 || die "make micro failed"
  mkdir -p "$STAGEDIR/micro"
  for m in vmexit vmexitstorm hello; do cp "build/micro/$m.bin" "$STAGEDIR/micro/"; done
  # The two must actually differ, and in the flag rather than only in ld.lld's timestamp.
  grep -aqc 'ENABLING (HYPE_ENABLE_AVIC set)' "$STAGEDIR/hype-avic.efi" >/dev/null 2>&1 || true
  if strings -a "$STAGEDIR/hype-default.efi" | grep -q 'ENABLING (HYPE_ENABLE_AVIC set)'; then
    die "the DEFAULT build contains the AVIC ENABLING string -- the variants got crossed"
  fi
  strings -a "$STAGEDIR/hype-avic.efi" | grep -q 'ENABLING (HYPE_ENABLE_AVIC set)' \
    || die "the AVIC build does NOT contain the ENABLING string -- EXTRA_CFLAGS did not take"
  if strings -a "$STAGEDIR/hype-default.efi" | grep -q '(HYPE_ENABLE_APICV set)'; then
    die "the DEFAULT build contains the APICv marker -- the variants got crossed"
  fi
  strings -a "$STAGEDIR/hype-apicv.efi" | grep -q '(HYPE_ENABLE_APICV set)' \
    || die "the APICv build does NOT contain its marker -- EXTRA_CFLAGS did not take"
fi

# ---------------------------------------------------------------- scratch images (#738)
# hype creates nothing. Each entry is "path-under-DATAMP:bytes". Sizes match the
# target_disk_size_gb in the configs that name them.
mk_image() {
  # Split: `local` expands ALL its arguments before any assignment takes effect, so
  # referencing $rel in the same statement that sets it is unbound under `set -u`.
  local rel=$1 want=$2 have=0
  local base=${3:-$DATAMP}
  local path="$base/$rel"
  mkdir -p "$(dirname "$path")"
  [ -f "$path" ] && have=$(stat -c %s "$path")
  # #816: an ext4 extent marked unwritten reads as zeros only through the host's ext4 driver.
  # hype reads the sectors directly and refuses such a file (#696), so a right-sized image made
  # by fallocate is still unusable: both 2026-09 i5 boots logged `ext4-scratch.img NOT FOUND`.
  if [ "$have" = "$want" ] && ! filefrag -v "$path" 2>/dev/null | grep -q unwritten; then
    echo "  ok    $rel ($want bytes)"; return 0
  fi
  if [ "$have" = "$want" ]; then
    echo "  REWRITE $rel: unwritten extents, which hype refuses (#696)"
  elif [ "$have" != 0 ]; then
    echo "  RESIZE $rel: $have -> $want bytes"
  else
    echo "  create $rel ($want bytes)"
  fi
  if [ "$CHECK" = 1 ]; then echo "  (--check: not creating)"; return 1; fi
  # Real zeros, never fallocate: fallocate leaves ext4 extents unwritten and exFAT's
  # ValidDataLength short (tools/make-disk-image.sh), and hype only ever writes in place.
  dd if=/dev/zero of="$path" bs=1M count=$((want / 1048576)) conv=fsync status=none \
      || die "could not create $rel"
  return 0
}
echo "scratch images (#738):"
IMG_RC=0
mk_image "hype/disks/run1a-scratch.img" $((2 * 1024 * 1024 * 1024)) || IMG_RC=1
mk_image "hype/disks/run2b-scratch.img" $((2 * 1024 * 1024 * 1024)) || IMG_RC=1
mk_image "hype/disks/run2c-scratch.img" $((2 * 1024 * 1024 * 1024)) || IMG_RC=1
if [ -n "$EXT4MP" ]; then
  # #688 / #689: the guest images and their ISOs live ONLY on ext4 / NTFS (see above).
  mk_image "hype/disks/ext4-scratch.img" $((1 * 1024 * 1024 * 1024)) "$EXT4MP" || IMG_RC=1
  mk_image "hype/disks/ntfs-scratch.img" $((1 * 1024 * 1024 * 1024)) "$NTFSMP" || IMG_RC=1
  if [ "$CHECK" = 0 ]; then
    ISO_SRC=$HERE/../../disk-images/hwval-data-2026-09-09/iso/test.iso
    [ -f "$ISO_SRC" ] || die "no Alpine ISO at $ISO_SRC"
    mkdir -p "$EXT4MP/iso" "$NTFSMP/iso"
    [ -f "$EXT4MP/iso/test.iso" ]      || cp "$ISO_SRC" "$EXT4MP/iso/test.iso"      || die "copy ISO to ext4"
    [ -f "$NTFSMP/iso/ntfs-test.iso" ] || cp "$ISO_SRC" "$NTFSMP/iso/ntfs-test.iso" || die "copy ISO to NTFS"
    [ ! -e "$DATAMP/iso/test.iso" ] || die "$DATAMP/iso/test.iso exists and would shadow the ext4 copy -- remove it"
  fi
  echo "ext4 data partition:"; ls -l "$EXT4MP/iso" "$EXT4MP/hype/disks" 2>/dev/null | sed 's/^/  /'
  echo "NTFS data partition:"; ls -l "$NTFSMP/iso" "$NTFSMP/hype/disks" 2>/dev/null | sed 's/^/  /'
fi

# ---------------------------------------------------------------- copy
if [ "$CHECK" = 0 ]; then
  echo "staging onto $BOOTMP ..."
  cp "$STAGEDIR/hype-default.efi" "$BOOTMP/EFI/hype/hype-default.efi" || die "copy default"
  cp "$STAGEDIR/hype-avic.efi"    "$BOOTMP/EFI/hype/hype-avic.efi"    || die "copy avic"
  cp "$STAGEDIR/hype-apicv.efi"   "$BOOTMP/EFI/hype/hype-apicv.efi"   || die "copy apicv"
  cp "$STAGEDIR/hype-$ACTIVE_BUILD.efi" "$BOOTMP/EFI/BOOT/BOOTX64.EFI" || die "copy active"
  cp $HERE/hype*.cfg "$BOOTMP/" || die "copy configs"
  [ -f "$HERE/$ACTIVE_CFG" ] || die "active config $ACTIVE_CFG not found"
  cp "$HERE/$ACTIVE_CFG" "$BOOTMP/hype.cfg" || die "copy active config"
  if [ -d "$STAGEDIR/micro" ]; then
    mkdir -p "$BOOTMP/EFI/hype/micro"
    cp "$STAGEDIR"/micro/*.bin "$BOOTMP/EFI/hype/micro/" || die "copy micro kernels"
  fi
  [ -f "$HERE/$RUN_CARD" ] || die "run card $RUN_CARD not found"
  cp "$HERE/$RUN_CARD" "$BOOTMP/RUN-CARD.md" || die "copy run card"
  # The active input script. Kept under its own name on the drive as well, so which run a
  # log belongs to is recoverable from the drive alone after the fact.
  [ -f "$HERE/$BOOT_INPUT/vm0.txt" ] || die "input script $BOOT_INPUT/vm0.txt not found"
  mkdir -p "$BOOTMP/input" "$BOOTMP/$BOOT_INPUT"
  # Every VM's script, not only vm0's: a stale \input\vm1.txt from an earlier set would
  # otherwise drive this set's second VM.
  rm -f "$BOOTMP"/input/vm*.txt
  for f in "$HERE/$BOOT_INPUT"/vm*.txt; do
    cp "$f" "$BOOTMP/input/$(basename "$f")"      || die "copy input script $(basename "$f")"
    cp "$f" "$BOOTMP/$BOOT_INPUT/$(basename "$f")" || die "copy input archive $(basename "$f")"
  done
  cp docs/hw-validation-queue-2026-09-09.md "$BOOTMP/QUEUE.md" 2>/dev/null || true
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
  verify "$STAGEDIR/hype-$ACTIVE_BUILD.efi" "$BOOTMP/EFI/BOOT/BOOTX64.EFI" "BOOTX64.EFI (= hype-$ACTIVE_BUILD.efi)"
  verify "$STAGEDIR/hype-default.efi" "$BOOTMP/EFI/hype/hype-default.efi" "hype-default.efi"
  verify "$STAGEDIR/hype-avic.efi"    "$BOOTMP/EFI/hype/hype-avic.efi"    "hype-avic.efi"
  verify "$STAGEDIR/hype-apicv.efi"   "$BOOTMP/EFI/hype/hype-apicv.efi"   "hype-apicv.efi"
fi
for c in $HERE/hype*.cfg; do
  verify "$c" "$BOOTMP/$(basename "$c")" "$(basename "$c")"
done
verify "$HERE/$ACTIVE_CFG" "$BOOTMP/hype.cfg" "hype.cfg (= $ACTIVE_CFG)"
if [ -d "$STAGEDIR/micro" ]; then
  for m in "$STAGEDIR"/micro/*.bin; do verify "$m" "$BOOTMP/EFI/hype/micro/$(basename "$m")" "micro/$(basename "$m")"; done
fi
if [ "$CHECK" = 0 ]; then
  for f in "$HERE/$BOOT_INPUT"/vm*.txt; do
    verify "$f" "$BOOTMP/input/$(basename "$f")" "input/$(basename "$f")"
  done
  verify "$HERE/$RUN_CARD" "$BOOTMP/RUN-CARD.md" "RUN-CARD.md"
fi
echo "scratch image on media:"
ls -l "$DATAMP/hype/disks/" 2>/dev/null | tail -n +2 | sed 's/^/  /'

# ---------------------------------------------------------------- leave the drive safe to pull
#
# #818: this script used to leave every filesystem it mounted still mounted. Pull the drive in
# that state and its ext4 keeps needs_recovery set on disk -- and hype's ext reader refuses such
# a volume outright (core/ext.c: "an unreplayed journal: nothing on disk can be trusted yet"),
# two sectors in. That is exactly what the i5 boots of 2026-09-13 16:10 and 17:28 hit: run2c and
# run2e found neither their ISO nor their disk and sat at "No bootable option or device was
# found", and the only thing said about it was "NOT FOUND on any of GPT partitions 1-4". FAT32,
# exFAT and NTFS have no equivalent flag, so the fault looked ext4-specific and build-related
# when it was neither.
echo "unmounting everything so the drive is safe to unplug ..."
sync
STILL=""
for d in "$BOOTDEV" "$DATADEV" ${EXT4DEV:+"$EXT4DEV"} ${NTFSDEV:+"$NTFSDEV"}; do
  [ -n "$d" ] || continue
  [ -n "$(mountpt "$d")" ] || continue
  udisksctl unmount -b "$d" >/dev/null 2>&1
  if [ -n "$(mountpt "$d")" ]; then STILL="$STILL $d"; else echo "  unmounted $d"; fi
done
if [ -n "$STILL" ]; then
  echo "  STILL MOUNTED:$STILL -- unmount these before unplugging, or hype will be handed a" >&2
  echo "  dirty volume and refuse it [#818]" >&2
  RC=1
fi

[ "$IMG_RC" = 0 ] || RC=1
if [ "$RC" = 0 ]; then echo "STAGED OK -- safe to unplug"; else echo "STAGING HAS PROBLEMS -- see above" >&2; fi
exit "$RC"
