#!/bin/bash
# #232: assemble hype-additions.iso -- a plain (non-bootable) data disc, unlike
# #228's boot-image-preserving Alpine remaster. It is attached as a SECOND
# cdroms entry (docs/hype-cfg-spec.md §5.7) alongside a stock OS installer
# ISO in cdroms[0]; nothing here needs to be bootable on its own.
#
#   /linux/apks-hype/x86_64/...   offline apk repo (linux/build-repo.sh)
#   /linux/install-linux.sh       unattended Alpine install driver
#   /bsd/installerconfig          FreeBSD's own auto-detected answer file
#   /windows/autounattend.xml     Windows Setup's own auto-detected answer file
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
HYPE_DISK_IMAGES=${HYPE_DISK_IMAGES:-$(cd "$HERE/../.." && pwd)/disk-images}
B=${HYPE_232_BUILD:-$HYPE_DISK_IMAGES/hype-232-build}
OUT=${HYPE_232_OUT_ISO:-$B/hype-additions.iso}

[ -d "$B/linux/apks-hype/x86_64" ] || {
    echo "no offline apk repo at $B/linux/apks-hype -- run linux/build-repo.sh first" >&2
    exit 1
}

STAGE="$B/iso-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/linux" "$STAGE/bsd" "$STAGE/windows"

cp -a "$B/linux/apks-hype" "$STAGE/linux/"
install -m 0755 "$HERE/linux/install-linux.sh" "$STAGE/linux/install-linux.sh"
install -m 0644 "$HERE/bsd/installerconfig" "$STAGE/bsd/installerconfig"
# bsdinstall's auto-detection scans for `installerconfig` at a medium's ROOT,
# not a subdirectory -- keep a top-level copy alongside the archived one above.
install -m 0644 "$HERE/bsd/installerconfig" "$STAGE/installerconfig"
install -m 0644 "$HERE/windows/autounattend.xml" "$STAGE/windows/autounattend.xml"
# Same reasoning: Windows Setup scans a medium's ROOT for autounattend.xml.
install -m 0644 "$HERE/windows/autounattend.xml" "$STAGE/autounattend.xml"

xorriso -as mkisofs -V HYPEADDONS -J -R -o "$OUT" "$STAGE"
echo "wrote $OUT"
du -sh "$OUT"
