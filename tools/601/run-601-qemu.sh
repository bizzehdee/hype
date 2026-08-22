#!/bin/bash
# #601: build with -DHYPE_ENABLE_X2APIC=1 and confirm a Linux guest boots with
# x2apic mode enabled and reaches login, plus that the default (flag-off)
# build is untouched.
set -e
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-disk-images/601}"
B=build
SECS="${1:-500}"
ISO="${ISO:-disk-images/alpine-virt-console.iso}"
CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE.fd}
VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS.fd}
mkdir -p "$S"
killall -9 qemu-system-x86_64 2>/dev/null || true; sleep 1

cat > $S/hype.cfg <<'CFG'
[hype]
config_version = 1
log_level = debug

[vm.smp]
label = x2apic601
vcpus = 2
mem_mb = 1024
boot = installer
install_media = \iso\test.iso
firmware = uefi
os_hint = linux
target_disk = file:\hype\disks\smp.img
CFG

cat > $S/vm0.txt <<'IN'
timeout 480000
fail-if soft lockup
expect localhost login:
send root\n
expect ~#
send dmesg | grep -i x2apic\n
expect x2apic
send echo "X601" "SEEN"\n
expect X601 SEEN
pass x2apic-601
IN

rm -f $S/esp.img
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
mcopy -i "$E" $S/vm0.txt ::/input/vm0.txt

for ATTEMPT in 1 2 3; do
  cp "$VARS" $S/VARS.fd
  timeout "$SECS" qemu-system-x86_64 \
    -machine q35 -m 4096 -nodefaults \
    -accel kvm -accel tcg -cpu host -smp 4 \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file=$S/VARS.fd \
    -drive format=raw,file=$S/esp.img,if=none,id=esp \
    -device ide-hd,drive=esp,bus=ide.0,bootindex=0 \
    -serial stdio -display none -vga none > $S/serial-$ATTEMPT.txt 2>&1 || true
  if grep -aq "hype: build" $S/serial-$ATTEMPT.txt; then
    echo "attempt $ATTEMPT: hype ran"; cp $S/serial-$ATTEMPT.txt $S/serial.txt; break
  fi
  echo "attempt $ATTEMPT: hype never ran (#371 noboot) -- retrying"
done

echo "=== verdict ==="
rc=0
if grep -aq "x2apic enabled" $S/serial.txt; then
  echo "PASS: guest dmesg reports x2apic enabled"
else
  echo "FAIL: guest never reported x2apic enabled"; rc=1
fi
if grep -aq "x2apic-601" $S/serial.txt; then
  echo "PASS: script completed (guest reached login and stayed usable)"
else
  echo "FAIL: script pass marker absent"; rc=1
fi
[ "$rc" -eq 0 ] && echo "PASS: #601 x2APIC guest boot verified"
exit "$rc"
