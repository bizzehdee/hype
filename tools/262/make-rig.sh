#!/bin/sh
# #262 repro rig: does hype's SATA/AHCI disk model produce a BOOTABLE disk for
# the guest firmware?
#
# The ticket's original repro needed a full install to a physical target, which
# means real hardware and the one authorised scratch disk. This does the same job
# in QEMU in about two minutes, because #199 (raw file-backed virtual disk) has
# since closed: a `boot = disk` guest can be pointed straight at a pre-made
# bootable image, with no install step and no physical disk at all.
#
# Two things this script exists to get right, both of which cost a wasted run
# when they were got wrong:
#
#  1. THE CONTROL RUNS FIRST. Bare QEMU must boot the generated image over SATA
#     before hype is given it. Without that, "the firmware refused it" and "the
#     image was never bootable" look identical -- and this ticket has already
#     spent three hypotheses on the guest-side model.
#
#  2. THE ESP IS A REAL FAT IMAGE, NOT vvfat. `-drive file=fat:rw:<dir>` makes
#     QEMU SIGSEGV once hype writes to the volume (reproduced: three coredumps,
#     zero serial output, looks exactly like hype failing to start). A real
#     partitioned FAT32 image is also what the machine actually has.
#
# Needs no root: sfdisk + mtools only.
set -eu
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=${OUT:-/mnt/data/hype-bisect/rig262}
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS.fd}
SHELL_ISO=${SHELL_ISO:-/usr/share/edk2/ovmf/UefiShell.iso}

mkdir -p "$OUT"
cd "$OUT"

# ---------------------------------------------------------------- guest payload
# The UEFI Shell, because it prints an unmistakable "Shell>" and a mapping table
# naming the device path it booted from -- so success says WHICH disk it used,
# rather than leaving us to infer it from the absence of the BDS failure message.
if [ ! -f BOOTX64.EFI ]; then
    rm -rf shelliso && mkdir shelliso && (cd shelliso && 7z x -y "$SHELL_ISO" >/dev/null)
    mcopy -i shelliso/uefi_shell.img ::/EFI/BOOT/BOOTX64.EFI ./BOOTX64.EFI
fi

# ------------------------------------------------------- the bootable test disk
# GPT + one FAT32 ESP + EFI/BOOT/BOOTX64.EFI at the removable-media fallback
# path, which is what BDS looks for without an NVRAM boot entry.
# Fully allocated, never sparse -- same reasoning as tools/make-disk-image.sh:
# hype's file-backed writer never grows a file, so a hole is not "unwritten", and
# a host FS asked to allocate on first write to a hole would fail the guest's
# write mid-run.
dd if=/dev/zero of=boot-test.img bs=1M count=128 status=none
sfdisk --label gpt boot-test.img >/dev/null <<'PART'
start=2048, size=253952, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="EFI System"
PART
dd if=/dev/zero of=esp.fat bs=512 count=253952 status=none
mformat -i esp.fat -F -v HYPEBOOT ::
mmd -i esp.fat ::/EFI ::/EFI/BOOT
mcopy -i esp.fat BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
dd if=esp.fat of=boot-test.img bs=512 seek=2048 conv=notrunc status=none

# ------------------------------------------------------------------ THE CONTROL
echo "== control: bare QEMU must boot boot-test.img over SATA =="
cp "$OVMF_VARS" ctrl-vars.fd
timeout 90 "$QEMU" -machine q35 -m 2048 -nodefaults -accel kvm -cpu host \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file=ctrl-vars.fd \
  -drive format=raw,file=boot-test.img,if=none,id=d0 -device ide-hd,drive=d0,bus=ide.0 \
  -serial file:ctrl.log -display none 2>/dev/null || true
if grep -q "Shell>" ctrl.log 2>/dev/null; then
    echo "   CONTROL PASS -- image is bootable over SATA:"
    tr -d '\r' < ctrl.log | grep -o "Sata([^)]*)/HD([^)]*)" | head -1
else
    echo "   CONTROL FAIL -- the image itself is not bootable; fix that before blaming hype."
    exit 1
fi

# ------------------------------------------------------------- hype's own ESP
# NOTE: the image goes at \hype\disks\vm0.img because that path is COMPILE-TIME
# fixed (HYPE_M5_8_IMAGE_PATH). hype.cfg's `target_disk = file:<path>` is parsed,
# echoed to the log, and then ignored -- see #285. Until that is fixed, putting
# the image anywhere else silently yields the 64 MiB RAM scratch instead, and the
# config echo makes it look like the setting took.
dd if=/dev/zero of=esp-hype.img bs=1M count=640 status=none
sfdisk --label gpt esp-hype.img >/dev/null <<'PART'
start=2048, size=1306624, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="EFI System"
PART
dd if=/dev/zero of=esp-hype.fat bs=512 count=1306624 status=none
mformat -i esp-hype.fat -F -v HYPEESP ::
mmd -i esp-hype.fat ::/EFI ::/EFI/BOOT ::/EFI/hype ::/hype ::/hype/disks
mcopy -i esp-hype.fat "$REPO/build/hype.efi" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i esp-hype.fat "$REPO/fw/OVMF_CODE.fd" "$REPO/fw/OVMF_VARS.fd" ::/EFI/hype/
mcopy -i esp-hype.fat boot-test.img ::/hype/disks/vm0.img
cat > hype.cfg <<'CFG'
[vm.boottest]
vcpus = 1
mem_mb = 1024
boot = disk
target_disk = file:\hype\disks\vm0.img
os_hint = linux
firmware = uefi
CFG
mcopy -i esp-hype.fat hype.cfg ::/hype.cfg
dd if=esp-hype.fat of=esp-hype.img bs=512 seek=2048 conv=notrunc status=none

echo "== rig ready =="
echo "   build hype first:  make clean && make all EXTRA_CFLAGS=\"-DHYPE_RUN_TWO_VMS=0 -DHYPE_FW_1_GUEST_RAM_MB=1024\""
echo "   then:              $HERE/run-rig.sh"
