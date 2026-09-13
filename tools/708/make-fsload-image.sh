#!/bin/bash
#
# Build the hw-val drive's layout as one QEMU usb-storage image: a GPT stick with
#
#   p1 FAT32  HYPEBOOT   hype, firmware, hype.cfg, input scripts, and hype's own log
#   p2 exFAT  HYPEDATA   \hype\disks\run2c-scratch.img
#   p3 ext4   HYPEEXT4   \iso\test.iso      + \hype\disks\ext4-scratch.img   (#688)
#   p4 NTFS   HYPENTFS   \iso\ntfs-test.iso + \hype\disks\ntfs-scratch.img   (#689)
#
# Split out of tools/708/run-708-fsload.sh so tools/820/run-820-typing.sh drives the same load
# without a second copy of it. Sourced, not run: it needs the caller's $S, $EFI and $CFG and
# leaves "$S"/usb.img, "$S"/target.img and "$S"/VARS.fd behind.
#
# udisks loop-mounts, so no root. Two traps this cost two rig runs to find, both now guarded:
# a default Fedora mkfs.ext4 (orphan_file, metadata_csum_seed) is unreadable to hype's ext
# driver, and a partition dd'd out while still mounted comes out "needs journal recovery",
# which hype refuses outright (core/ext.c INCOMPAT_RECOVER).
set -u
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"

EFI="${EFI:-rig/stage-current/hype-default.efi}"
S="${SCRATCH:-rig/708-fsload}"
ISO=disk-images/hwval-data-2026-09-09/iso/test.iso
HERE=tools/hw-val-2026-08-25
CFG="${CFG:-$HERE/hype2h.cfg}"
[ -f "$EFI" ] || { echo "no $EFI"; exit 2; }
[ -f "$ISO" ] || { echo "no ISO at $ISO"; exit 2; }

# udisks loop-mounts, so no root: the ext4 image is made with root_owner=1000:1000 and the
# exFAT/NTFS ones mount as the caller anyway. debugfs write is NOT used -- it lays a big file
# down in ~100 extents and hype's resolver caps at 64 (see the ext4/FAT test-volume recipe).
MOUNTED=()
MP=""
# Sets MP, and does NOT echo it: the first cut was called as `MP=$(fs_mount ...)`, and a command
# substitution is a subshell, so every `MOUNTED+=(...)` was thrown away with it. Nothing was ever
# unmounted, each dd copied a still-mounted filesystem, and the assembled ext4 came out
# "needs journal recovery" -- which hype's ext driver will not read, so run2c and run2e were lost
# with only "NOT FOUND on any of GPT partitions 1-4" to say so.
fs_mount() { # $1 = image; sets MP
    local l dev m
    MP=""
    l=$(udisksctl loop-setup -f "$1" --no-user-interaction) || return 1
    dev=$(echo "$l" | grep -oE '/dev/loop[0-9]+')
    [ -n "$dev" ] || return 1
    MOUNTED+=("$dev")
    # udisks may already have auto-mounted the new loop device; asking again is an error.
    for _ in 1 2 3 4 5; do
        MP=$(lsblk -no MOUNTPOINT "$dev" | head -1)
        [ -n "$MP" ] && break
        sleep 1
    done
    if [ -z "$MP" ]; then
        m=$(udisksctl mount -b "$dev" --no-user-interaction) || return 1
        MP=$(echo "$m" | sed -E 's/^Mounted .* at //; s/\.$//')
    fi
    [ -n "$MP" ]
}
fs_umount_all() {
    local dev
    for dev in "${MOUNTED[@]:-}"; do
        [ -n "$dev" ] || continue
        udisksctl unmount -b "$dev" --no-user-interaction >/dev/null 2>&1
        udisksctl loop-delete -b "$dev" --no-user-interaction >/dev/null 2>&1
    done
    MOUNTED=()
}
trap fs_umount_all EXIT

rm -rf "$S"; mkdir -p "$S"

# ---- p1 FAT32: hype, firmware, config, input scripts (mtools, no mount needed) -------------
dd if=/dev/zero of="$S"/p1.img bs=1M count=640 status=none
mformat -i "$S"/p1.img -F -v HYPEBOOT ::
mmd -i "$S"/p1.img ::/EFI ::/EFI/BOOT ::/EFI/hype ::/input
mcopy -i "$S"/p1.img "$EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$S"/p1.img fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i "$S"/p1.img "$CFG" ::/hype.cfg
for v in 0 1 2 3; do
    [ -f "$HERE/input-2h/vm$v.txt" ] && mcopy -i "$S"/p1.img "$HERE/input-2h/vm$v.txt" "::/input/vm$v.txt"
done

# ---- a fully allocated 1 GiB scratch image, copied into each data volume ------------------
dd if=/dev/zero of="$S"/zero1g.img bs=1M count=1024 status=none

# ---- p2 exFAT: run2c's image --------------------------------------------------------------
dd if=/dev/zero of="$S"/p2.img bs=1M count=1100 status=none
mkfs.exfat -L HYPEDATA "$S"/p2.img >/dev/null
fs_mount "$S"/p2.img || { echo "p2 mount failed"; exit 2; }
mkdir -p "$MP/hype/disks"; cp "$S"/zero1g.img "$MP/hype/disks/run2c-scratch.img"
sync; fs_umount_all

# ---- p3 ext4: the #688 ISO and image -------------------------------------------------------
dd if=/dev/zero of="$S"/p3.img bs=1M count=1700 status=none
# -O ^orphan_file,^metadata_csum_seed: this repo's standing mkfs.ext4 options (#495-#498, #507,
# #508). A default Fedora volume carries both, and hype's ext driver does not read it -- the
# first cut of this rig lost run2c AND run2e that way, with only "NOT FOUND on any of GPT
# partitions 1-4" to say so.
mkfs.ext4 -q -F -b 4096 -O ^orphan_file,^metadata_csum_seed -E root_owner=1000:1000 \
    -L HYPEEXT4 "$S"/p3.img
fs_mount "$S"/p3.img || { echo "p3 mount failed"; exit 2; }
mkdir -p "$MP/iso" "$MP/hype/disks"
cp "$ISO" "$MP/iso/test.iso"; cp "$S"/zero1g.img "$MP/hype/disks/ext4-scratch.img"
sync; fs_umount_all

# ---- p4 NTFS: the #689 ISO and image -------------------------------------------------------
dd if=/dev/zero of="$S"/p4.img bs=1M count=1700 status=none
mkfs.ntfs -q -f -F -L HYPENTFS "$S"/p4.img >/dev/null
fs_mount "$S"/p4.img || { echo "p4 mount failed"; exit 2; }
mkdir -p "$MP/iso" "$MP/hype/disks"
cp "$ISO" "$MP/iso/ntfs-test.iso"; cp "$S"/zero1g.img "$MP/hype/disks/ntfs-scratch.img"
sync; fs_umount_all
sync
# The assembled ext4 must be CLEAN. A volume dd'd out while still mounted comes out
# "needs journal recovery", and hype's ext driver will not read it -- that cost this rig two
# whole runs, both of which looked like "the images just were not there".
if dumpe2fs -h "$S"/p3.img 2>/dev/null | grep -q needs_recovery; then
    echo "p3 was not cleanly unmounted -- refusing to assemble a dirty ext4"; exit 2
fi
rm -f "$S"/zero1g.img

# ---- assemble the whole drive ---------------------------------------------------------------
dd if=/dev/zero of="$S"/usb.img bs=1M count=5150 conv=fsync status=none
sfdisk --label gpt -q "$S"/usb.img <<'SFDISK'
start=2048,      size=1310720, type=uefi,  name=HYPEBOOT
start=1312768,   size=2252800, type=linux, name=HYPEDATA
start=3565568,   size=3481600, type=linux, name=HYPEEXT4
start=7047168,   size=3481600, type=linux, name=HYPENTFS
SFDISK
dd if="$S"/p1.img of="$S"/usb.img bs=512 seek=2048    conv=notrunc status=none
dd if="$S"/p2.img of="$S"/usb.img bs=512 seek=1312768 conv=notrunc status=none
dd if="$S"/p3.img of="$S"/usb.img bs=512 seek=3565568 conv=notrunc status=none
dd if="$S"/p4.img of="$S"/usb.img bs=512 seek=7047168 conv=notrunc status=none
rm -f "$S"/p1.img "$S"/p2.img "$S"/p3.img "$S"/p4.img

# run3d's physical target: present so the config resolves as it does on the drive; run3d is
# not autostarted, so nothing is written to it.
dd if=/dev/zero of="$S"/target.img bs=1M count=256 status=none
cp /usr/share/edk2/ovmf/OVMF_VARS.fd "$S"/VARS.fd

