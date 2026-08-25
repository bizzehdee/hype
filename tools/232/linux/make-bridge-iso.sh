#!/bin/bash
# #232: remaster alpine-standard with ONLY a two-step handoff apkovl --
# unlike #228's remaster, no offline repo or install seed is injected onto
# this medium. This becomes cdroms[0] in a hype VM; the separate
# hype-additions.iso (build-additions-iso.sh) is cdroms[1] and carries
# everything install-linux.sh actually needs.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
HYPE_DISK_IMAGES=${HYPE_DISK_IMAGES:-$(cd "$HERE/../../.." && pwd)/disk-images}
B=${HYPE_232_BUILD:-$HYPE_DISK_IMAGES/hype-232-build}
BASE=${HYPE_232_BASE_ISO:-$HYPE_DISK_IMAGES/alpine-standard-3.21.7-x86_64.iso}
OUT=${HYPE_232_BRIDGE_ISO:-$B/alpine-hype-232-bridge.iso}

[ -f "$BASE" ] || { echo "base ISO not found: $BASE" >&2; exit 1; }

rm -rf "$B/bridge-ovl"
mkdir -p "$B/bridge-ovl/etc/local.d" "$B/bridge-ovl/etc/runlevels/default"
install -m 0755 "$HERE/bridge-boot.start" "$B/bridge-ovl/etc/local.d/bridge-boot.start"
ln -sf /etc/init.d/local "$B/bridge-ovl/etc/runlevels/default/local"
( cd "$B/bridge-ovl" && tar -czf "$B/hype-bridge.apkovl.tar.gz" etc )

mkdir -p "$B/bridge-inj/boot/grub"
# Same serial-console cmdline #228 already proved works -- hype's whole
# diagnostic pipeline needs ttyS0, and this is not the thing to re-discover.
cat > "$B/bridge-inj/boot/grub/grub.cfg" <<'EOF'
set timeout=1
serial --unit=0 --speed=115200
terminal_input serial console
terminal_output serial console

menuentry "Linux lts (hype #232 additions-ISO bridge)" {
linux	/boot/vmlinuz-lts modules=loop,squashfs,sd-mod,usb-storage console=ttyS0,115200
initrd	/boot/initramfs-lts
}
EOF

rm -f "$OUT"
xorriso -indev "$BASE" -outdev "$OUT" \
    -boot_image any replay \
    -map "$B/bridge-inj/boot/grub/grub.cfg" /boot/grub/grub.cfg \
    -map "$B/hype-bridge.apkovl.tar.gz" /hype-bridge.apkovl.tar.gz \
    -commit

if ! xorriso -indev "$OUT" -toc 2>&1 | grep -q "El Torito"; then
    echo "FATAL: $OUT has no El Torito boot record -- it will not UEFI-boot" >&2
    exit 1
fi
echo "built $OUT ($(stat -c%s "$OUT") bytes), El Torito preserved"
