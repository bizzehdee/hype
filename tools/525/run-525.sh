#!/bin/bash
# #525: prove the ACPI reset register (0xCF9) guest-reboot path from a NON-BSP
# vCPU -- SVM leg (QEMU/KVM on AMD). One 2-vCPU Alpine guest, an input script
# that pins the kernel's reboot CPU to 1 and reboots, a USB log volume so the
# per-VM logs capture the run. The verdict is hype's own attribution line:
# "vm0 vCPU 1 guest reset via ACPI reset register (0xCF9)" -- exactly once --
# plus the input script's pass marker (login reached again after the restart).
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-disk-images/525}"
B=build
SECS="${1:-600}"
ISO="${ISO:-disk-images/alpine-virt-console.iso}"
CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE.fd}
VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS.fd}
mkdir -p "$S"
killall -9 "$(basename "$QEMU")" 2>/dev/null || true; sleep 1

rm -f $S/usb.img $S/esp.img
dd if=/dev/zero of=$S/usb.img bs=1M count=64 status=none
mkfs.vfat -F 32 -n HYPEUSB $S/usb.img >/dev/null

cat > $S/hype.cfg <<'CFG'
[hype]
config_version = 1

[vm.smp]
label = smp525
vcpus = 2
mem_mb = 1024
boot = installer
install_media = \iso\test.iso
firmware = uefi
os_hint = linux
target_disk = file:\hype\disks\smp.img
CFG

# GPT, not superfloppy: a bare-FAT image kills the media resolver (it walks GPT
# partitions 1-4 for the \iso file lookup) -- the resolver then reports only
# "no partition 2 (raw ISO)" and the installer VM has no boot media.
dd if=/dev/zero of=$S/esp.img bs=1M count=512 status=none
sfdisk -q $S/esp.img <<'PT'
label: gpt
start=2048, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B
PT
E="$S/esp.img@@1M"
mkfs.vfat -F 32 -n HYPEESP --offset 2048 $S/esp.img >/dev/null
mmd -i "$E" ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/input ::/hype ::/hype/disks
mcopy -i "$E" $B/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$E" fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i "$E" "$ISO" ::/iso/test.iso
mcopy -i "$E" $S/hype.cfg ::/hype.cfg
mcopy -i "$E" tools/input-scripts/smp525-reboot-vm0.txt ::/input/vm0.txt

for ATTEMPT in 1 2 3; do
  cp "$VARS" $S/VARS.fd
  timeout "$SECS" "$QEMU" \
    -machine q35 -m 4096 -nodefaults \
    -accel kvm -accel tcg -cpu host -smp 4 \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file=$S/VARS.fd \
    -drive format=raw,file=$S/esp.img,if=none,id=esp \
    -device ide-hd,drive=esp,bus=ide.0,bootindex=0 \
    -device qemu-xhci,id=xhci \
    -drive format=raw,file=$S/usb.img,if=none,id=stick \
    -device usb-storage,bus=xhci.0,drive=stick \
    -serial stdio -display none -vga none > $S/serial-$ATTEMPT.txt 2>&1 || true
  if grep -aq "hype: build" $S/serial-$ATTEMPT.txt; then
    echo "attempt $ATTEMPT: hype ran"; cp $S/serial-$ATTEMPT.txt $S/serial.txt; break
  fi
  echo "attempt $ATTEMPT: hype never ran (#371 noboot) -- retrying"
done

echo "=== verdict ==="
rc=0
resets=$(grep -ac "vCPU 1 guest reset via ACPI reset register (0xCF9)" $S/serial.txt || true)
echo "non-BSP (vCPU 1) 0xCF9 resets: $resets"
if [ "$resets" != "1" ]; then echo "FAIL: expected exactly 1 non-BSP reset, saw $resets"; rc=1; fi
if grep -aq "vCPU 0 guest reset via ACPI reset register" $S/serial.txt; then
  echo "FAIL: a reset was served by the BSP vCPU -- the pin did not hold"; rc=1
fi
sync
if grep -aq "smp525-nonbsp-reboot" $S/serial.txt; then
  echo "input script: PASS (guest restarted once and reached login again)"
else
  echo "FAIL: input script pass marker absent -- restart never completed"; rc=1
fi
[ "$rc" -eq 0 ] && echo "PASS: 0xCF9 served from vCPU 1, one clean restart to login"
exit "$rc"
