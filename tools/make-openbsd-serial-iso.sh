#!/bin/bash
# #318: give an OpenBSD install ISO a SERIAL console, WITHOUT remastering it.
#
#   tools/make-openbsd-serial-iso.sh <install ISO> [output ISO]
#
# OpenBSD's kernel does not inherit the EFI console and the shipped ISO puts the console on
# video; hype models no VGA text device, so without this the guest cannot speak at all (#318).
#
# WHY IN-PLACE PATCHING, NOT xorriso: OpenBSD's boot loader (sys/lib/libsa/cd9660.c) has NO
# Rock Ridge support and matches plain ISO9660 names by UPPERCASING the path -- so the stored
# name must be uppercase, while the release directory is the illegal-but-intact `7.9` that
# OpenBSD masters deliberately. Rewriting the image cannot satisfy both:
#   * default compliance normalises `7.9` -> `7_9`, so every kernel path ENOENTs;
#   * -compliance untranslated_name_len=-1 keeps `7.9` but also keeps `etc` LOWERCASE, so
#     /etc/boot.conf ENOENTs, the console never switches, and the loader falls back to /bsd.
# Both failures were observed on real hardware. So: copy the original byte-for-byte and
# overwrite ONLY the bytes of /etc/boot.conf, extending its directory-record size field when
# the new text is longer. Every name in every namespace stays exactly as OpenBSD wrote it.
set -eu
SRC="${1:?usage: $0 <install ISO> [output ISO]}"
OUT="${2:-${SRC%.iso}-serial.iso}"
[ -f "$SRC" ] || { echo "no such ISO: $SRC"; exit 1; }

cp -f "$SRC" "$OUT"
python3 - "$OUT" <<'PY'
import sys, struct
path = sys.argv[1]
SEC = 2048

def records(f, lba, length):
    f.seek(lba * SEC); data = f.read(length); off = 0
    while off < len(data):
        l = data[off]
        if l == 0:
            off += 1; continue
        nl = data[off + 32]
        yield (off, l, int.from_bytes(data[off+2:off+6], 'little'),
               int.from_bytes(data[off+10:off+14], 'little'), data[off+33:off+33+nl])
        off += l

with open(path, 'r+b') as f:
    # Primary Volume Descriptor at LBA 16; its root directory record is at byte 156.
    f.seek(16 * SEC + 156)
    rr = f.read(34)
    root_lba = int.from_bytes(rr[2:6], 'little')
    root_len = int.from_bytes(rr[10:14], 'little')

    etc = None
    for off, l, ext, size, name in records(f, root_lba, root_len):
        if name.upper() == b'ETC':
            etc = (ext, size); break
    if etc is None:
        sys.exit("no /etc directory in the ISO9660 namespace")

    target = None
    for off, l, ext, size, name in records(f, etc[0], etc[1]):
        if name.upper().split(b';')[0] == b'BOOT.CONF':
            target = (etc[0] * SEC + off, ext, size, name); break
    if target is None:
        sys.exit("no /etc/boot.conf in the ISO9660 namespace")
    rec_pos, ext, size, name = target

    # Carry the release path across from the ORIGINAL boot.conf rather than assuming 7.9.
    f.seek(ext * SEC); orig = f.read(size)
    image = b''
    for line in orig.split(b'\n'):
        if line.startswith(b'set image'):
            image = line + b'\n'
    new = b'stty com0 115200\nset tty com0\n' + image
    if len(new) > SEC:
        sys.exit("serial boot.conf exceeds one sector")

    f.seek(ext * SEC); f.write(new + b'\n' * (SEC - len(new)))   # pad the rest of the sector
    f.seek(rec_pos + 10); f.write(struct.pack('<I', len(new)))    # data length (LE)
    f.seek(rec_pos + 14); f.write(struct.pack('>I', len(new)))    # data length (BE mirror)
    print(f"patched /etc/boot.conf: extent {ext}, {size} -> {len(new)} bytes, name {name!r}")
PY

# Verify from the OUTPUT image, and prove the namespace was NOT touched.
xorriso -osirrox on -indev "$OUT" -extract /etc/boot.conf /tmp/hype-boot.conf >/dev/null 2>&1
grep -q '^set tty com0' /tmp/hype-boot.conf || { echo "FAILED: serial console not in $OUT"; exit 1; }
grep -q '^set image' /tmp/hype-boot.conf || { echo "FAILED: set image line lost"; exit 1; }
for iso in "$SRC" "$OUT"; do
    isoinfo -l -i "$iso" 2>/dev/null | sed -n '/Directory listing of \/$/,/^$/p' | \
        awk '/\[/ {print $NF}' | grep -v '^\.\.\?$' | sort > "/tmp/hype-ns-$(basename "$iso").txt"
done
cmp -s "/tmp/hype-ns-$(basename "$SRC").txt" "/tmp/hype-ns-$(basename "$OUT").txt" || \
    { echo "FAILED: the ISO9660 root namespace CHANGED -- the loader matches these names literally"; exit 1; }
echo "wrote $OUT ($(stat -c%s "$OUT") bytes); namespace unchanged; /etc/boot.conf now:"
sed 's/^/    /' /tmp/hype-boot.conf
