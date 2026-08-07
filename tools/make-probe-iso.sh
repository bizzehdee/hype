#!/bin/sh
# #358: a bootable UEFI ISO that probes the guest's own block devices and prints what it finds.
#
# The point is to distinguish "the firmware never connected a driver" from "it connected one and
# got nothing": UEFI binds drivers lazily, so a device missing from `map` is NOT by itself evidence
# that the device is broken. startup.nsh forces `connect -r` FIRST, then maps and dumps sector 0 of
# every block device, so the tagged test images identify which slot each handle really is.
#
# Needs the shell's El Torito FAT image from an existing UefiShell.iso; it is reused rather than
# rebuilt so the shell binary is the distro's, not something assembled here.
set -eu
out="${1:-build}"
shell_iso="${2:-/usr/share/edk2/ovmf/UefiShell.iso}"
mkdir -p "$out"
rm -f "$out/uefi_shell.img" "$out/probe.iso"
xorriso -osirrox on -indev "$shell_iso" -extract /uefi_shell.img "$out/uefi_shell.img" >/dev/null 2>&1
chmod +w "$out/uefi_shell.img"
mcopy -o -i "$out/uefi_shell.img" tools/qemu-probe-startup.nsh ::/startup.nsh
xorriso -as mkisofs -quiet -V HYPEPROBE -e uefi_shell.img -no-emul-boot \
  -o "$out/probe.iso" -graft-points "/uefi_shell.img=$out/uefi_shell.img"
echo "$out/probe.iso built"
