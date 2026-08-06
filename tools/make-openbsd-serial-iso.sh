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
# Three details that each cost an attempt (the third cost the whole first #318 investigation):
#
#   * `-boot_image any replay` is REQUIRED. Without it xorriso writes an image with no El Torito
#     catalog and the result does not boot -- which looks like a hype regression, not a bad ISO.
#   * `stty` must come BEFORE `set tty`, and the original `set image` line must be kept. The shipped
#     /etc/boot.conf is exactly one line (`set image /7.9/amd64/bsd.rd`), so it is read out of the
#     source ISO rather than hardcoded here -- a different release numbers its path differently.
#   * `-compliance untranslated_name_len=-1` is REQUIRED. OpenBSD's loader cd9660.c has NO Rock
#     Ridge support (its own header says so) and matches plain uppercase ISO9660 names, so OpenBSD
#     deliberately masters its ISOs with the ILLEGAL directory name `7.9` intact. On rewrite,
#     xorriso normalises that to the compliant `7_9` and moves `7.9` into a Rock Ridge NM record
#     the loader cannot read -- so `boot> booting cd0a:/7.9/amd64/bsd.rd` fails with "No such file
#     or directory" on the remaster while /etc/boot.conf (whose path survives case-folding) still
#     works. That asymmetry is exactly the #318 symptom, and it is made HERE, not in hype.
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

xorriso -compliance untranslated_name_len=-1 \
        -indev "$SRC" -outdev "$OUT" -boot_image any replay \
        -map "$TMP/boot.conf" /etc/boot.conf -commit >/dev/null 2>&1

# Verify rather than trust: a silently unmodified ISO would send the next reader hunting hype.
xorriso -osirrox on -indev "$OUT" -extract /etc/boot.conf "$TMP/check.conf" >/dev/null 2>&1
grep -q '^set tty com0' "$TMP/check.conf" || { echo "FAILED: serial console not in $OUT"; exit 1; }
# The loader reads plain ISO9660 names, so the release directory (the first component of the
# `set image` path, e.g. `7.9`) must survive UNTRANSLATED -- `7_9` here means the loader will
# ENOENT on every kernel path and the guest never leaves boot>. Checked against the PLAIN
# listing (no -R): the Rock Ridge listing shows `7.9` even when the ISO name was mangled.
RELDIR="$(grep -h '^set image' "$TMP/check.conf" | sed 's|^set image /||; s|/.*||')"
if [ -n "$RELDIR" ]; then
    isoinfo -l -i "$OUT" 2>/dev/null | sed -n '/Directory listing of \/$/,/^$/p' | grep -qF " $RELDIR " || \
        { echo "FAILED: release dir '$RELDIR' was translated in the ISO9660 namespace -- loader cannot boot this"; exit 1; }
fi
echo "wrote $OUT ($(stat -c%s "$OUT") bytes); /etc/boot.conf now:"
sed 's/^/    /' "$TMP/check.conf"
