#!/bin/bash
# #522: two guests on one emulated USB stick, so the log drain sees a two-VM
# production rate with a real \HYPE.LOG sink. tools/338's rig is single-VM and
# never falls behind, which is why the drain's burst behaviour was only ever
# visible on hardware.
set -e
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:?set SCRATCH to a DISK-backed dir, never /tmp (tmpfs)}"
B=build
SECS="${1:-240}"
ISO="${ISO:-disk-images/alpine-virt-console.iso}"

mkdir -p "$S"
rm -f $S/usb.img $S/esp.img
dd if=/dev/zero of=$S/usb.img bs=1M count=64 status=none
mkfs.vfat -F 32 -n HYPEUSB $S/usb.img >/dev/null

cat > $S/hype.cfg <<'CFG'
[hype]
config_version = 1

[vm.a]
label = drain load A
vcpus = 1
mem_mb = 1024
boot = installer
install_media = \iso\test.iso
firmware = uefi
os_hint = linux
target_disk = file:\hype\disks\a.img

[vm.b]
label = drain load B
vcpus = 1
mem_mb = 1024
boot = installer
install_media = \iso\vm1.iso
firmware = uefi
os_hint = linux
target_disk = file:\hype\disks\b.img
CFG

dd if=/dev/zero of=$S/esp.img bs=1M count=2048 status=none
mkfs.vfat -F 32 -n HYPEESP $S/esp.img >/dev/null
mmd -i $S/esp.img ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/hype ::/hype/disks
mcopy -i $S/esp.img $B/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i $S/esp.img fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i $S/esp.img "$ISO" ::/iso/test.iso
mcopy -i $S/esp.img "$ISO" ::/iso/vm1.iso
# NOCFG=1 reproduces a stick that ships NO hype.cfg at all, which then takes the built-in defaults
# -- including HYPE_SMP_STARTABLE_VCPUS for the vCPU count. That knob has had exactly one job since
# #192: raising the no-config default. With a config present it is INERT, because vcpu_count is
# admission-validated and is the authority (see fw_1_guest_visible_vcpus).
#
# So this flag is about the no-config path only. tools/hwstick/ does ship a hype.cfg now, so a
# hardware stick built from it needs no -D at all -- #527's build line said otherwise and was stale;
# see the correction on that ticket. A rig that always ships a cfg still cannot catch a no-config
# regression, which is why this switch exists.
if [ -z "${NOCFG:-}" ]; then mcopy -i $S/esp.img $S/hype.cfg ::/hype.cfg; fi

for ATTEMPT in 1 2 3; do
  cp /usr/share/OVMF/OVMF_VARS.fd $S/VARS.fd
  timeout "$SECS" qemu-system-x86_64 \
    -machine q35 -m 6144 -nodefaults \
    -accel kvm -accel tcg -cpu host -smp 4 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
    -drive if=pflash,format=raw,file=$S/VARS.fd \
    -drive format=raw,file=$S/esp.img,if=none,id=esp \
    -device ide-hd,drive=esp,bus=ide.0,bootindex=0 \
    -device qemu-xhci,id=xhci \
    -drive format=raw,file=$S/usb.img,if=none,id=stick \
    -device usb-storage,bus=xhci.0,drive=stick \
    -serial stdio -display none -vga none > $S/serial-$ATTEMPT.txt 2>&1 || true
  # #371: a run that never reached hype is a noboot, not a result.
  if grep -aq "hype: build" $S/serial-$ATTEMPT.txt; then
    echo "attempt $ATTEMPT: hype ran"; cp $S/serial-$ATTEMPT.txt $S/serial.txt; break
  fi
  echo "attempt $ATTEMPT: hype never ran (#371 noboot) -- retrying"
done

echo "=== files left on the USB stick ==="
mdir -i $S/usb.img :: || true
echo "=== USBFLUSH tail ==="
grep -a "USBFLUSH" $S/serial.txt | tail -5 || true
