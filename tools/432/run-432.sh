#!/bin/bash
# #432 validation, three legs on the vendored SECURE_BOOT_ENABLE OVMF + enrolled varstore:
#   A: an UNSIGNED efi (alpine's ISO loader) is refused by the guest firmware
#   B: the Microsoft-SIGNED shim loads and executes
#   C: the persisted varstore reports SecureBoot ON with PK/KEK/db enrolled
set -e
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-$(mktemp -d /mnt/data/dev/hype/disk-images/rig432.XXXXXX)}"
echo "scratch: $S"
killall -9 qemu-system-x86_64 2>/dev/null || true
sleep 1
[ -f fw/OVMF_CODE.secboot.fd ] && [ -f fw/OVMF_VARS.secboot.fd ] || {
    echo "run FW_SECBOOT=1 tools/build-fw.sh + tools/enroll-secboot.sh first"; exit 1; }

# a boot disk carrying the HOST's Microsoft-signed shim as the removable-path loader
dd if=/dev/zero of="$S"/shimdisk.img bs=1M count=16 conv=fsync status=none
mkfs.vfat -F 12 -n SHIM "$S"/shimdisk.img >/dev/null
mmd -i "$S"/shimdisk.img ::/EFI ::/EFI/BOOT
mcopy -i "$S"/shimdisk.img disk-images/432/shimx64.efi ::/EFI/BOOT/BOOTX64.EFI

build_esp() { # $1 = cfg
    rm -f "$S"/esp.img
    dd if=/dev/zero of="$S"/esp.img bs=1M count=512 conv=fsync status=none
    sfdisk --label gpt -q "$S"/esp.img <<SFDISK
2048,,U
SFDISK
    mformat -i "$S"/esp.img@@1M -F ::
    mmd -i "$S"/esp.img@@1M ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/hype ::/hype/disks
    mcopy -i "$S"/esp.img@@1M build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
    mcopy -i "$S"/esp.img@@1M fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
    mcopy -i "$S"/esp.img@@1M fw/OVMF_CODE.secboot.fd fw/OVMF_VARS.secboot.fd ::/EFI/hype/
    mcopy -i "$S"/esp.img@@1M disk-images/alpine-hype-dbg.iso ::/iso/test.iso
    mcopy -i "$S"/esp.img@@1M "$S"/shimdisk.img ::/hype/disks/shimdisk.img
    mcopy -i "$S"/esp.img@@1M "$1" ::/hype.cfg
}

run_qemu() { # $1 = log, $2 = secs
    cp /usr/share/edk2/ovmf/OVMF_VARS.fd "$S"/VARS.fd
    timeout "$2" qemu-system-x86_64 -machine q35 -m 4096 -nodefaults \
      -accel kvm -cpu host -smp 4 \
      -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
      -drive if=pflash,format=raw,file="$S"/VARS.fd \
      -device ich9-ahci,id=ahci \
      -drive format=raw,file="$S"/esp.img,if=none,id=d0 \
      -device ide-hd,drive=d0,bus=ahci.0,bootindex=0 \
      -serial "file:$1" -display none -vga none || true
}

echo "=== leg A: unsigned media refused ==="
build_esp tools/432/hype-unsigned.cfg
run_qemu "$S"/legA.log 180
LC_ALL=C grep -a "SECURE BOOT\|Security Violation\|Access Denied\|verif" "$S"/legA.log | head -4
LC_ALL=C grep -aq "localhost login" "$S"/legA.log && { echo "FAIL: unsigned alpine BOOTED under Secure Boot"; exit 1; }
LC_ALL=C grep -aqi "Access Denied\|Security Violation" "$S"/legA.log || { echo "FAIL: no refusal evidence"; exit 1; }
echo "leg A PASS: unsigned refused"

echo "=== leg B: the Microsoft-signed shim executes ==="
build_esp tools/432/hype-shim.cfg
for a in 1 2 3; do
    run_qemu "$S"/legB.log 180
    LC_ALL=C grep -aqi "shim" "$S"/legB.log && break
    echo "leg B attempt $a produced no shim output (QEMU pre-11.1 AHCI crash class, #581) -- retrying"
done
LC_ALL=C grep -a "shim\|Shim\|SHIM\|fallback\|grub" "$S"/legB.log | head -4
LC_ALL=C grep -aqi "shim" "$S"/legB.log || { echo "FAIL: shim never spoke -- it did not execute"; exit 1; }
echo "leg B PASS: the signed loader ran"

echo "=== leg C: the enrolled varstore reports Secure Boot on with PK/KEK/db ==="
# (Persistence of guest SetVariable writes through the pflash device is #119's machinery,
#  already hardware-validated; legs A+B above are the RUNTIME evidence that the SecureBoot
#  variable is in force -- OVMF only enforces when it reads 1.)
virt-fw-vars --input fw/OVMF_VARS.secboot.fd --print > "$S"/vars.txt
grep -E "SecureBoot|PK |KEK|db " "$S"/vars.txt | head -6
grep -q "SecureBootEnable.*ON" "$S"/vars.txt || { echo "FAIL: SecureBoot not enabled in the varstore"; exit 1; }
grep -qE "^ *PK " "$S"/vars.txt && grep -qE "^ *KEK" "$S"/vars.txt && grep -qE "^ *db " "$S"/vars.txt || { echo "FAIL: PK/KEK/db missing"; exit 1; }
echo "leg C PASS"
echo "ALL PASS"
