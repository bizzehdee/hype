#!/bin/bash
# Put the exFAT content back after rebuild-drive-2026-09-09.sh. No root: udisks mounts.
# \iso\test.iso deliberately does NOT go back on exFAT -- it would shadow the ext4 copy
# (the resolver takes the first partition holding the path). stage.sh puts it on ext4/NTFS.
set -eu
SRC=/mnt/data/dev/hype/disk-images/hwval-data-2026-09-09
DEV=$(lsblk -rpno NAME,LABEL | awk '$2=="HYPEDATA"{print $1; exit}')
[ -n "$DEV" ] || { echo "no HYPEDATA volume -- run rebuild-drive-2026-09-09.sh first" >&2; exit 1; }
MP=$(lsblk -no MOUNTPOINT "$DEV" | head -1)
[ -n "$MP" ] || { udisksctl mount -b "$DEV" >/dev/null; MP=$(lsblk -no MOUNTPOINT "$DEV" | head -1); }
mkdir -p "$MP/hype/disks" "$MP/iso"
rsync -a --info=progress2 "$SRC/hype/" "$MP/hype/"
rsync -a --info=progress2 --exclude test.iso "$SRC/iso/" "$MP/iso/"
sync; ls -l "$MP/iso" "$MP/hype/disks"
