#!/bin/sh
# Builds a real-hardware test package for hype.efi (AGENTS.md's
# mandatory real-hardware validation pass -- QEMU alone is necessary
# but not sufficient). Produces two equivalent forms of the same
# removable-media UEFI layout (EFI/BOOT/BOOTX64.EFI -- the path
# firmware looks for automatically on removable media, no NVRAM boot
# entry needed):
#
#   release/usb/          a plain directory tree -- copy its contents
#                          onto a USB drive you've already formatted
#                          FAT32 yourself (file manager, or `cp -r`).
#   release/hype-usb.img   a raw, already-FAT32-formatted disk image,
#                          built entirely via mtools (mformat/mmd/
#                          mcopy) -- this script never touches a real
#                          block device or mounts anything; only you
#                          decide when/whether to `dd`/Rufus/
#                          balenaEtcher this onto an actual USB drive.
#
# Run from anywhere; paths are all relative to this script's own
# location, same convention as tools/build-fw.sh.
set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
RELEASE_DIR="$REPO_ROOT/release"
OUT_DIR="$RELEASE_DIR/usb"
IMG_PATH="$RELEASE_DIR/hype-usb.img"
IMG_SIZE_MB=64

for tool in mformat mmd mcopy dd; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "make-usb-package.sh: required tool '$tool' not found on PATH (mtools package)." >&2
        exit 1
    fi
done

echo "make-usb-package.sh: building hype.efi..."
make -C "$REPO_ROOT"

echo "make-usb-package.sh: writing directory tree to $OUT_DIR..."
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/EFI/BOOT"
cp "$REPO_ROOT/build/hype.efi" "$OUT_DIR/EFI/BOOT/BOOTX64.EFI"
cp "$SCRIPT_DIR/usb-package-README.md" "$OUT_DIR/README.md"

echo "make-usb-package.sh: building $IMG_PATH (FAT32, ${IMG_SIZE_MB}MB)..."
mkdir -p "$RELEASE_DIR"
rm -f "$IMG_PATH"
dd if=/dev/zero of="$IMG_PATH" bs=1M count="$IMG_SIZE_MB" status=none
mformat -i "$IMG_PATH" -F ::
mmd -i "$IMG_PATH" ::/EFI
mmd -i "$IMG_PATH" ::/EFI/BOOT
mcopy -i "$IMG_PATH" "$REPO_ROOT/build/hype.efi" ::/EFI/BOOT/BOOTX64.EFI

echo
echo "make-usb-package.sh: done."
echo "  Directory tree (copy onto an already-FAT32 USB drive): $OUT_DIR"
echo "  Raw flashable image (dd/Rufus/balenaEtcher):           $IMG_PATH"
echo "  Read $OUT_DIR/README.md before booting real hardware with this."
