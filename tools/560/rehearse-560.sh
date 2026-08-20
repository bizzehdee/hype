#!/bin/bash
# #560: rehearse the REAL hardware-validation stick under QEMU, on the topology that binds.
#
#   tools/560/rehearse-560.sh [seconds] [smp-spec]
#
# The default SMP spec is the AMD laptop's actual shape -- 4 physical cores, 2 threads each.
# `-smp 8` is NOT a substitute: it gives eight single-threaded cores, so the SMT placement path
# never runs and a config that fits only because of #560 appears to fit for the wrong reason.
#
# topoext=on is REQUIRED on an AMD host: without it QEMU warns "This family of AMD CPU doesn't
# support hyperthreading(2)" and the guest cannot see SMT siblings at all, so the run silently
# rehearses the flat topology while claiming to rehearse the SMT one.
#
# What this checks: that no VM is dropped, that the placement lines say what the config claims,
# and that both Alpine input scripts arm. It does NOT check the guests' own tickets -- that is
# what the bare-metal run is for.
set -e
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
SECS="${1:-150}"
SMP="${2:-cpus=8,cores=4,threads=2}"
ISO="${ISO:-disk-images/alpine-virt-console.iso}"
S=rig/560   # artifacts on disk, never under build/ (make clean is rm -rf) and never tmpfs
STICK=$S/stick

[ -f build/hype.efi ] || { echo "build/hype.efi missing -- run 'make all'"; exit 2; }
[ -f "$ISO" ] || { echo "no ISO at $ISO"; exit 2; }

mkdir -p "$S"
rm -rf "$STICK"; mkdir -p "$STICK/iso" "$STICK/hype/disks"

# THE MEDIA GOES ON BEFORE stage.sh RUNS, and the order is load-bearing now. stage.sh's last check
# refuses to finish when a VM has no boot device -- which is right, and since vm1 became a
# `boot = disk` guest (#120) a blank stick trips it. Staging first and populating afterwards meant
# the stage exited 1 with its output discarded, so the rehearsal died silently: RC=1, zero bytes.
# Caught by making that check fatal, which is the check earning its keep on its first run.


cp "$ISO" "$STICK/iso/test.iso"
# The target_disk files must EXIST. hype refuses to substitute a scratch disk for a missing one
# ("refusing to substitute a scratch disk"), which is correct -- silently inventing a disk is how
# an install lands somewhere nobody chose. stage.sh makes the directory but not the images, so a
# freshly staged stick has no vdisks until something creates them.
dd if=/dev/zero of="$STICK/hype/disks/vm0.img" bs=1M count="${VDISK_MB:-64}" status=none

# vm1 boots its OWN DISK now (#120), so a blank image is not a disk it can boot -- it is a VM that
# does not start. The artefact is built and control-booted once and then cached, because the builder
# spends up to two minutes proving in bare QEMU that the image reaches a login prompt, and paying
# that on every rehearsal would discourage rehearsing.
VM1_DISK="${VM1_DISK:-build/guestdisk/alpine-disk.img}"
if [ ! -f "$VM1_DISK" ]; then
    echo "building vm1's boot disk once (control-booted, then cached at $VM1_DISK)"
    tools/make-guest-disk-from-iso.sh "$ISO" "$VM1_DISK" || {
        echo "vm1's boot disk could not be built -- see above. Not rehearsing a stick that cannot"
        echo "start vm1: the ticket riding on it (#120) would produce no evidence."
        exit 1
    }
fi
cp "$VM1_DISK" "$STICK/hype/disks/vm1-alpine-disk.img"

tools/hwstick/stage.sh "$STICK"

# The image MUST be GPT-partitioned with partition 1 = FAT32, not a bare FAT filesystem: hype
# locates the volume with hype_gpt_find_partition() before handing it to core/fat.c. A bare
# `mkfs.vfat` image has no partition for it to find, and the only symptom is hype refusing to
# resolve target_disk and the ISOs -- which reads like a config problem, not a rig problem.
# tools/run-guest.sh's build_esp_file() carries the same warning; this rig made the mistake anyway.
MB=1024
rm -f $S/esp.img $S/part1.img
fallocate -l "$((MB + 1))M" $S/esp.img 2>/dev/null || \
    dd if=/dev/zero of=$S/esp.img bs=1M count=$((MB + 1)) status=none conv=fsync
sfdisk --label gpt -q $S/esp.img >/dev/null <<SFDISK
2048,,U
SFDISK
dd if=/dev/zero of=$S/part1.img bs=1M count="$MB" status=none
mkfs.vfat -F 32 -n HYPEHW $S/part1.img >/dev/null
(cd "$STICK" && mcopy -s -i "$OLDPWD/$S/part1.img" ./* ::/)
dd if=$S/part1.img of=$S/esp.img bs=1M seek=1 conv=notrunc,fsync status=none
rm -f $S/part1.img
sync $S/esp.img
# Verify through the partition offset, the same way hype will read it. Trusting dd turns a short
# write into a boot mystery.
mdir -i "$S/esp.img@@1M" ::/EFI/BOOT 2>/dev/null | grep -q BOOTX64 || \
    { echo "ESP verify FAILED: BOOTX64.EFI not readable at partition 1"; exit 1; }
mdir -i "$S/esp.img@@1M" ::/iso 2>/dev/null | grep -qi "test" || \
    { echo "ESP verify FAILED: \\iso\\test.iso not readable at partition 1"; exit 1; }
# vm1's boot disk, verified through the partition offset for the same reason: a short write here
# leaves a VM with no boot device, and the only symptom is a guest that never appears.
mdir -i "$S/esp.img@@1M" ::/hype/disks 2>/dev/null | grep -qi "alpine" || \
    { echo "ESP verify FAILED: vm1's boot disk not readable at partition 1"; exit 1; }

killall -9 qemu-system-x86_64 2>/dev/null || true
sleep 1

for ATTEMPT in 1 2 3; do
  # The OUTER firmware's varstore must be the HOST's, never fw/OVMF_VARS.fd -- that pair is the
  # GUEST firmware hype hands its VMs, and using it here boots to silent zero output.
  cp /usr/share/edk2/ovmf/OVMF_VARS.fd $S/VARS.fd
  timeout $((SECS + 60)) qemu-system-x86_64 \
    -machine q35 -m 12288 -nodefaults \
    -accel kvm -accel tcg -cpu host,topoext=on -smp "$SMP" \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
    -drive if=pflash,format=raw,file=$S/VARS.fd \
    -drive format=raw,file=$S/esp.img,if=none,id=esp \
    -device ide-hd,drive=esp,bus=ide.0,bootindex=0 \
    -serial stdio -display none -vga std > $S/serial-$ATTEMPT.txt 2>&1 || true
  # #371: a boot that never reached hype is a noboot, not a result.
  if grep -aq "hype: build" $S/serial-$ATTEMPT.txt; then
    cp $S/serial-$ATTEMPT.txt $S/serial.txt
    echo "attempt $ATTEMPT: hype ran"
    break
  fi
  echo "attempt $ATTEMPT: hype never ran (#371 noboot, $(wc -c <$S/serial-$ATTEMPT.txt) bytes) -- retrying"
done
killall -9 qemu-system-x86_64 2>/dev/null || true

echo "=== VMs dropped (must be none):"
grep -a "WILL NOT RUN\|only the first" $S/serial.txt || echo "  none"
echo "=== placement:"
grep -a "fw-1 SMP:" $S/serial.txt || echo "  NO PLACEMENT LINES -- investigate"
echo "=== guest media resolved (empty means the guests got none):"
grep -a "host-fat: resolved\|NOT FOUND on any of GPT" $S/serial.txt | head -4 || true
echo "=== input scripts armed:"
grep -a "input: vm[01] armed" $S/serial.txt || echo "  NEITHER ARMED -- investigate"
