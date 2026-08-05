#!/bin/bash
# #318: remaster an OpenBSD install ISO to use the SERIAL console.
#
#   tools/make-openbsd-serial-iso.sh <install ISO> [output ISO]
#
# OpenBSD's kernel does not inherit the EFI console, and the shipped ISO puts the console on video.
# hype models no VGA text device (devices/ has bochs_vbe.c and ramfb.c only) and the harness runs
# -display none, so WITHOUT this the guest cannot say anything at all -- the last output of a whole
# run is the loader's "entry point at ...", which is why #318 had nothing to go on.
#
# Two details that each cost an attempt:
#
#   * `-boot_image any replay` is REQUIRED. Without it xorriso writes an image with no El Torito
#     catalog and the result does not boot -- which looks like a hype regression, not a bad ISO.
#   * `stty` must come BEFORE `set tty`, and the original `set image` line must be kept. The shipped
#     /etc/boot.conf is exactly one line (`set image /7.9/amd64/bsd.rd`), so it is read out of the
#     source ISO rather than hardcoded here -- a different release numbers its path differently.
set -eu
SRC="${1:?usage: $0 <install ISO> [output ISO]}"
OUT="${2:-${SRC%.iso}-serial.iso}"
[ -f "$SRC" ] || { echo "no such ISO: $SRC"; exit 1; }
command -v xorriso >/dev/null || { echo "xorriso is required"; exit 1; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

# Carry the source's own `set image` line across rather than assuming the release layout.
xorriso -osirrox on -indev "$SRC" -extract /etc/boot.conf "$TMP/orig.conf" >/dev/null 2>&1 || true
{
    echo 'stty com0 115200'
    echo 'set tty com0'
    grep -h '^set image' "$TMP/orig.conf" 2>/dev/null || true
} > "$TMP/boot.conf"

xorriso -indev "$SRC" -outdev "$OUT" -boot_image any replay \
        -map "$TMP/boot.conf" /etc/boot.conf -commit >/dev/null 2>&1

# Verify rather than trust: a silently unmodified ISO would send the next reader hunting hype.
xorriso -osirrox on -indev "$OUT" -extract /etc/boot.conf "$TMP/check.conf" >/dev/null 2>&1
grep -q '^set tty com0' "$TMP/check.conf" || { echo "FAILED: serial console not in $OUT"; exit 1; }
echo "wrote $OUT ($(stat -c%s "$OUT") bytes); /etc/boot.conf now:"
sed 's/^/    /' "$TMP/check.conf"
