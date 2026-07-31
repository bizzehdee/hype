#!/bin/bash
# #228: assemble the seeded unattended-install Alpine ISO.
#
# Injects three things into a stock alpine-standard ISO, preserving its UEFI boot
# image (xorriso `-boot_image any replay`) so hype's guest OVMF can still boot it:
#
#   /apks-hype            offline apk repo   (build-offline-repo.sh)
#   /hype.apkovl.tar.gz   autoinstall overlay (autoinstall.start)
#   /boot/grub/grub.cfg   stock cmdline + a serial console
#
# The grub cmdline keeps alpine-standard's module list VERBATIM. Changing it was
# tried and is NOT what fixes the modloop problem -- see autoinstall.start.
set -euo pipefail

B=${HYPE_228_BUILD:-$HOME/Downloads/hype-228-build}
BASE=${HYPE_228_BASE_ISO:-$HOME/Downloads/alpine-standard-3.21.7-x86_64.iso}
OUT=${HYPE_228_OUT_ISO:-$B/alpine-hype-228.iso}
HERE=$(cd "$(dirname "$0")" && pwd)

[ -f "$BASE" ] || { echo "base ISO not found: $BASE" >&2; exit 1; }
[ -d "$B/apks-hype/x86_64" ] || { echo "no offline repo; run build-offline-repo.sh first" >&2; exit 1; }

# The apkovl is a tarball of /etc that Alpine's initramfs picks up off the boot
# medium ("Loading user settings from /media/sr0/hype.apkovl.tar.gz").
rm -rf "$B/ovl"
mkdir -p "$B/ovl/etc/local.d" "$B/ovl/etc/runlevels/default"
install -m 0755 "$HERE/autoinstall.start" "$B/ovl/etc/local.d/autoinstall.start"
ln -sf /etc/init.d/local "$B/ovl/etc/runlevels/default/local"
( cd "$B/ovl" && tar -czf "$B/hype.apkovl.tar.gz" etc )

mkdir -p "$B/inj/boot/grub"
cat > "$B/inj/boot/grub/grub.cfg" <<'EOF'
set timeout=1
serial --unit=0 --speed=115200
terminal_input serial console
terminal_output serial console

menuentry "Linux lts (hype #228 unattended install)" {
linux	/boot/vmlinuz-lts modules=loop,squashfs,sd-mod,usb-storage console=ttyS0,115200
initrd	/boot/initramfs-lts
}
EOF
cp "$B/hype.apkovl.tar.gz" "$B/inj/"

# HYPE_228_FORCE=1 drops the re-wipe override INSIDE the ISO. It has to live here,
# not on hype's ESP: autoinstall.start looks for it on the GUEST's boot medium
# (/media/sr0, /media/cdrom), and the guest never sees the USB stick hype booted
# from -- it sees only this CD and its target disk.
if [ "${HYPE_228_FORCE:-0}" = "1" ]; then
    : > "$B/inj/HYPE228_FORCE"
    echo "make-install-iso: HYPE228_FORCE included -- the re-wipe guard will be overridden"
else
    rm -f "$B/inj/HYPE228_FORCE"
fi
rm -rf "$B/inj/apks-hype"
cp -a "$B/apks-hype" "$B/inj/"

rm -f "$OUT"
xorriso -indev "$BASE" -outdev "$OUT" \
    -boot_image any replay \
    -map "$B/inj/boot/grub/grub.cfg" /boot/grub/grub.cfg \
    -map "$B/inj/hype.apkovl.tar.gz" /hype.apkovl.tar.gz \
    -map "$B/inj/apks-hype" /apks-hype \
    $( [ -f "$B/inj/HYPE228_FORCE" ] && printf -- '-map %s /HYPE228_FORCE' "$B/inj/HYPE228_FORCE" ) \
    -commit

# A missing El Torito record means the guest OVMF will not boot it -- fail loudly
# here rather than after a wasted cold boot on real hardware.
if ! xorriso -indev "$OUT" -toc 2>&1 | grep -q "El Torito"; then
    echo "FATAL: $OUT has no El Torito boot record -- it will not UEFI-boot" >&2
    exit 1
fi
echo "built $OUT ($(stat -c%s "$OUT") bytes), El Torito preserved"
