#!/bin/bash
#
# Build an ALREADY-INSTALLED guest disk from a hybrid ISO, and prove it boots before shipping it.
#
#   tools/make-guest-disk-from-iso.sh <iso> <out.img>
#
# WHY THIS EXISTS. Every guest on the validation stick boots from installer MEDIA, which leaves the
# case M9-5 (#120) is about untested: a VM whose OS lives on its OWN disk, brought up by the guest
# firmware's own boot path, with nothing attached to stream from. The project has never had an
# installed-guest artefact, which is why that ticket keeps stalling.
#
# WHAT THIS IS, STATED PLAINLY. A modern Alpine ISO is a HYBRID image: it carries an MBR partition
# of type 0xEF (EFI System Partition) and a GPT, which is exactly why writing one to a USB stick
# produces a bootable disk. Presented to a guest as a plain disk, its firmware finds that ESP and
# boots it -- no media, no ISO streaming, the disk-boot path end to end.
#
# It is therefore a bootable installed disk in the sense #120 tests -- disk versus media -- and NOT
# the product of a `setup-disk` install. The guest runs Alpine's diskless/live mode off its own
# disk. Anything that specifically needs a partitioned, persistent install (#126, #164) needs a real
# install and is not served by this. Said out loud here rather than left for someone to discover
# from a passing run.
#
# THE CONTROL RUNS FIRST, which is #262's lesson: bare QEMU must boot the image before hype is ever
# given it. Without that, "hype's disk model refused it" and "the image was never bootable" look
# identical -- and on a cold-boot-only laptop that mistake costs a whole run.
#
set -u
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"

ISO="${1:-}"
OUT="${2:-}"
[ -n "$ISO" ] && [ -n "$OUT" ] || { echo "usage: $0 <iso> <out.img>"; exit 2; }
[ -f "$ISO" ] || { echo "no such ISO: $ISO"; exit 2; }
OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/ovmf/OVMF_CODE.fd}"
OVMF_VARS="${OVMF_VARS:-/usr/share/edk2/ovmf/OVMF_VARS.fd}"
[ -f "$OVMF_CODE" ] || { echo "no host OVMF at $OVMF_CODE"; exit 2; }

# ---- 1. is this ISO actually bootable as a DISK? -----------------------------
# A non-hybrid ISO copies fine and then produces a guest whose firmware finds no boot device, which
# reads exactly like a hype fault. Refuse here, where it is free.
python3 - "$ISO" <<'PY' || exit 1
import struct, sys
f = open(sys.argv[1], 'rb')
d = f.read(1024)
if d[510:512] != b'\x55\xaa':
    print("REFUSED: no MBR signature -- this ISO is not a hybrid image and will not boot as a disk")
    sys.exit(1)
esp = [i for i in range(4)
       if d[446 + i*16 + 4] == 0xEF]
if not esp:
    print("REFUSED: no MBR partition of type 0xEF (EFI System Partition).")
    print("  The guest's firmware would find nothing to boot. Use a hybrid/UEFI ISO.")
    sys.exit(1)
f.seek(512)
gpt = f.read(8) == b'EFI PART'
i = esp[0]
start, size = struct.unpack('<II', d[446 + i*16 + 8: 446 + i*16 + 16])
print("hybrid OK: ESP is MBR partition %d at LBA %d (%d sectors), GPT %s"
      % (i + 1, start, size, "present" if gpt else "ABSENT"))
PY

# ---- 2. the image ------------------------------------------------------------
# A plain copy, fully allocated. hype's file-backed writer never GROWS a file (#199), and a host FS
# asked to allocate on first write to a hole would fail the guest's write mid-run -- so a sparse
# copy is not an option here even though the bytes would be identical.
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
cp --sparse=never "$ISO" "$OUT"
sz=$(stat -c%s "$OUT")
echo "wrote $OUT ($((sz / 1048576)) MiB, fully allocated)"
[ "$sz" = "$(stat -c%s "$ISO")" ] || { echo "FAILED: short copy"; exit 1; }

# ---- 3. THE CONTROL --------------------------------------------------------
echo "=== control: bare QEMU must boot $OUT as a DISK (no cdrom attached) ==="
CTRL_DIR=$(dirname "$OUT")
cp "$OVMF_VARS" "$CTRL_DIR/.ctrl-vars.fd"
rm -f "$CTRL_DIR/.ctrl.log"
# if=none + ide-hd, and NO -cdrom: if it boots, it booted the disk. `console=ttyS0` is not needed --
# Alpine's ISO grub config already carries a serial console entry, and the login prompt lands on it.
timeout 120 "$QEMU" \
  -machine q35 -m 2048 -nodefaults -accel kvm -cpu host \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$CTRL_DIR/.ctrl-vars.fd" \
  -drive format=raw,file="$OUT",if=none,id=d0 -device ide-hd,drive=d0,bus=ide.0 \
  -serial "file:$CTRL_DIR/.ctrl.log" -display none -vga std >/dev/null 2>&1 || true

if grep -aq "login:" "$CTRL_DIR/.ctrl.log" 2>/dev/null; then
    echo "   CONTROL PASS -- reached a login prompt from the disk alone:"
    grep -ao "Welcome to Alpine[^|]\{0,30\}\|[a-z0-9]* login:" "$CTRL_DIR/.ctrl.log" \
        | sort -u | head -3 | sed 's/^/     /'
    rm -f "$CTRL_DIR/.ctrl-vars.fd"
    echo
    echo "Ready to stage. In hype.cfg give the VM:"
    echo "    boot = disk"
    echo "    target_disk = file:\\hype\\disks\\$(basename "$OUT")"
    echo "  and NO install_media -- with media attached, \"did it boot the disk or the media?\" is"
    echo "  unanswerable, and that distinction is the whole point of #120."
    exit 0
fi

echo "   CONTROL FAIL -- the image did not reach a login prompt in 120s."
echo "   $(wc -c < "$CTRL_DIR/.ctrl.log" 2>/dev/null || echo 0) bytes of serial in $CTRL_DIR/.ctrl.log"
echo "   Do NOT stage this: hype would be blamed for an image that was never bootable."
exit 1
