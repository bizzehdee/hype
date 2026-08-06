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
# ISO-1/ISO-2's own test guests read a real ISO from \iso\test.iso on
# the same volume hype.efi was booted from (boot/main.c) -- without
# one present, that test (and every test dispatched after it,
# including FW-1) fails immediately with a file-not-found panic before
# ever running. Same default the dev `make run` target already uses
# (Makefile's own TEST_ISO); override on the command line
# (TEST_ISO=/path/to.iso tools/make-usb-package.sh) if your distro
# vendors edk2-ovmf's own UefiShell.iso elsewhere or you'd rather use a
# different (small) ISO9660 image.
TEST_ISO=${TEST_ISO:-/usr/share/edk2/ovmf/UefiShell.iso}

for tool in mformat mmd mcopy dd; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "make-usb-package.sh: required tool '$tool' not found on PATH (mtools package)." >&2
        exit 1
    fi
done

# EXTRA_CFLAGS is passed through so a validation stick can select a build
# variant (e.g. -DHYPE_RUN_SELFTEST_GUESTS=1 to run the M2-M5 battery AND the
# live guest in ONE cold boot, which is what a serial-less machine needs).
#
# A `make clean` first when EXTRA_CFLAGS is set is deliberate and not
# belt-and-braces: make does NOT track changes to the compiler flags, so a tree
# already built with different EXTRA_CFLAGS is "up to date" and the flags are
# silently ignored. That has already cost a wasted hardware run -- the stick
# looked freshly built and carried the previous variant. A validation image is
# exactly where that must not happen, so correctness wins over build time here.
if [ -n "$EXTRA_CFLAGS" ]; then
    echo "make-usb-package.sh: EXTRA_CFLAGS set ($EXTRA_CFLAGS) -- clean rebuild"
    echo "  (make does not track flag changes; a stale object would ship the wrong variant)"
    make -C "$REPO_ROOT" clean >/dev/null
fi
echo "make-usb-package.sh: building hype.efi..."
make -C "$REPO_ROOT" EXTRA_CFLAGS="$EXTRA_CFLAGS"

echo "make-usb-package.sh: writing directory tree to $OUT_DIR..."
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/EFI/BOOT"
# #348: pre-allocate the log LARGE. hype's post-EBS FAT writer is in-place-only, so the log's
# capacity is the file's creation-time cluster chain -- a small or absent file silently truncates
# every run's diagnostics (this cost most of the 2026-08-06 hardware runs). 8MB outlasts any boot.
dd if=/dev/zero of="$OUT_DIR/HYPEFULL.LOG" bs=1M count=8 status=none
cp "$REPO_ROOT/build/hype.efi" "$OUT_DIR/EFI/BOOT/BOOTX64.EFI"
cp "$SCRIPT_DIR/usb-package-README.md" "$OUT_DIR/README.md"
# FW-1: hype.efi reads its own vendored guest firmware pair from
# \EFI\hype\ on the same volume it was itself booted from (core/
# file_io.h) -- must be present here for the real-OVMF-guest launch to
# find them, same path the QEMU `make run` target already copies these
# to (see Makefile's own `run` target).
mkdir -p "$OUT_DIR/EFI/hype"
cp "$REPO_ROOT/fw/OVMF_CODE.fd" "$REPO_ROOT/fw/OVMF_VARS.fd" "$OUT_DIR/EFI/hype/"

# INPUT-8 (#280): optional per-VM scripted input, read from \input\vmN.txt.
# On a serial-less machine this is what turns "it seemed to boot" into a
# self-checking PASS/FAIL verdict in the log, so a validation stick usually
# wants it. Absent = no scripted input, which is the normal default.
if [ -n "$INPUT_VM0" ] || [ -n "$INPUT_VM1" ]; then
    mkdir -p "$OUT_DIR/input"
    [ -n "$INPUT_VM0" ] && cp "$INPUT_VM0" "$OUT_DIR/input/vm0.txt"
    [ -n "$INPUT_VM1" ] && cp "$INPUT_VM1" "$OUT_DIR/input/vm1.txt"
fi

if [ -f "$TEST_ISO" ]; then
    mkdir -p "$OUT_DIR/iso"
    cp "$TEST_ISO" "$OUT_DIR/iso/test.iso"
else
    echo "make-usb-package.sh: WARNING: TEST_ISO ($TEST_ISO) not found -- ISO-1/ISO-2's own test" >&2
    echo "  guest (and everything dispatched after it, including FW-1) will panic immediately" >&2
    echo "  with a file-not-found error instead of actually running. Set TEST_ISO=/path/to.iso" >&2
    echo "  to point at a real ISO9660 image if you want the full test sequence to run." >&2
fi

# Size the FAT32 image to fit its contents: hype.efi + the two OVMF blobs
# (~8MB) + FAT metadata, plus the test ISO if present (which can be tens of
# MB -- a full Alpine ISO overflowed the old fixed 64MB). Base overhead +
# ISO size (rounded up to a whole MB), floored at IMG_SIZE_MB.
img_overhead_mb=32
iso_mb=0
if [ -f "$TEST_ISO" ]; then
    iso_bytes=$(wc -c < "$TEST_ISO")
    iso_mb=$(( (iso_bytes + 1048575) / 1048576 ))
fi
img_needed_mb=$(( img_overhead_mb + iso_mb ))
if [ "$img_needed_mb" -gt "$IMG_SIZE_MB" ]; then
    IMG_SIZE_MB=$img_needed_mb
fi

echo "make-usb-package.sh: building $IMG_PATH (FAT32, ${IMG_SIZE_MB}MB)..."
mkdir -p "$RELEASE_DIR"
rm -f "$IMG_PATH"
dd if=/dev/zero of="$IMG_PATH" bs=1M count="$IMG_SIZE_MB" status=none
mformat -i "$IMG_PATH" -F ::
mmd -i "$IMG_PATH" ::/EFI
mmd -i "$IMG_PATH" ::/EFI/BOOT
mmd -i "$IMG_PATH" ::/EFI/hype
mcopy -i "$IMG_PATH" "$REPO_ROOT/build/hype.efi" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$IMG_PATH" "$REPO_ROOT/fw/OVMF_CODE.fd" ::/EFI/hype/OVMF_CODE.fd
mcopy -i "$IMG_PATH" "$REPO_ROOT/fw/OVMF_VARS.fd" ::/EFI/hype/OVMF_VARS.fd
if [ -n "$INPUT_VM0" ] || [ -n "$INPUT_VM1" ]; then
    mmd -i "$IMG_PATH" ::/input
    [ -n "$INPUT_VM0" ] && mcopy -i "$IMG_PATH" "$INPUT_VM0" ::/input/vm0.txt
    [ -n "$INPUT_VM1" ] && mcopy -i "$IMG_PATH" "$INPUT_VM1" ::/input/vm1.txt
fi
if [ -f "$TEST_ISO" ]; then
    mmd -i "$IMG_PATH" ::/iso
    mcopy -i "$IMG_PATH" "$TEST_ISO" ::/iso/test.iso
fi

# Deliberately NO hype.cfg. That file is what arms a physical-target write, so a
# validation stick must not carry one: booting this image can never touch a real
# disk. Adding a hype.cfg is a separate, explicit decision (see #207/#210).

echo
echo "make-usb-package.sh: done."
echo "  Directory tree (copy onto an already-FAT32 USB drive): $OUT_DIR"
echo "  Raw flashable image (dd/Rufus/balenaEtcher):           $IMG_PATH"
echo "  Read $OUT_DIR/README.md before booting real hardware with this."
