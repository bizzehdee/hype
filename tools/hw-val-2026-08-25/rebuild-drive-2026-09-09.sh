#!/bin/bash
# Rebuild the hw-val drive's data side for docs/hw-validation-queue-2026-09-09.md:
#   p1  HYPEBOOT FAT32 4 GiB   -- KEPT, untouched
#   p2  exFAT   100 GiB        -- stage.sh's data partition (scratch images, the big ISOs)
#   p3  ext4    180 GiB        -- #688: \iso\test.iso + \hype\disks\ext4-scratch.img
#   p4  NTFS    rest (~190 GiB)-- #689: \iso\ntfs-test.iso + \hype\disks\ntfs-scratch.img
#
# DESTROYS p2 and everything after it. The old p2's content was copied to
# disk-images/hwval-data-2026-09-09/ (11 GB, rsync rc=0, 2026-09-09) before this was written;
# restore-drive-2026-09-09.sh puts the exFAT part of it back without root.
#
# The drive is found BY SERIAL, never by /dev letter. Run as root:
#   sudo tools/hw-val-2026-08-25/rebuild-drive-2026-09-09.sh
set -eu
SERIAL=115E0735191800123920   # SPCC Solid State Disk in the SABRENT USB-SATA enclosure
DISK=$(lsblk -dnpo NAME,SERIAL | awk -v s="$SERIAL" '$2==s{print $1; exit}')
[ -n "$DISK" ] || { echo "no disk with serial $SERIAL attached" >&2; exit 1; }
[ "$(lsblk -dno TRAN "$DISK")" = usb ] || { echo "$DISK is not on USB -- refusing" >&2; exit 1; }
lsblk -no LABEL "${DISK}1" | grep -qx HYPEBOOT || { echo "${DISK}1 is not HYPEBOOT -- refusing" >&2; exit 1; }
echo "rebuilding $DISK (serial $SERIAL):"; lsblk -o NAME,SIZE,FSTYPE,LABEL "$DISK"
for p in "$DISK"?*; do umount "$p" 2>/dev/null || true; done
parted -s "$DISK" rm 2
parted -s -a optimal "$DISK" \
  mkpart hypedata 4098MiB 106498MiB \
  mkpart hypeext4 106498MiB 290818MiB \
  mkpart hypentfs 290818MiB 100%
partprobe "$DISK"; sleep 2
mkfs.exfat -L HYPEDATA "${DISK}2"
mkfs.ext4 -q -F -L HYPEEXT4 -E root_owner=1000:1000 "${DISK}3"
mkfs.ntfs -q -f -L HYPENTFS "${DISK}4"
sync
echo "done:"; lsblk -o NAME,SIZE,FSTYPE,LABEL,UUID "$DISK"
echo "next, as the normal user: tools/hw-val-2026-08-25/restore-drive-2026-09-09.sh && tools/hw-val-2026-08-25/stage.sh --boot intelb"
