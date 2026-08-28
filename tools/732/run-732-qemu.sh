#!/bin/bash
# #732 validation: `[hype] autostart = keep, nosuchvm` with TWO VMs configured.
#
# PASS requires all four, because the bug was silence as much as it was behaviour:
#   1. 'keep' starts,
#   2. 'held' is left OFF and says why,
#   3. the unmatched name 'nosuchvm' is reported as a warning, not a refusal
#      (plan.md section 10 decision 72), and the boot continues,
#   4. 'held' can still be brought up by hand -- autostart must gate the BOOT
#      decision, not disable the VM.
# Before the fix, 'held' booted its own firmware and guest like any other VM.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S=rig/i732
SECS="${1:-240}"
mkdir -p $S
rm -f $S/serial.log $S/esp.img

killall -9 "$(basename "$QEMU")" 2>/dev/null || true
sleep 1
pidof qemu-system-x86_64 >/dev/null && { echo "FAIL: a qemu is still running"; exit 1; }

dd if=/dev/zero of=$S/esp.img bs=1M count=768 status=none
mkfs.vfat -F 32 -n HYPEESP $S/esp.img >/dev/null
mmd -i $S/esp.img ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/hype ::/hype/disks
mcopy -i $S/esp.img build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i $S/esp.img fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i $S/esp.img disk-images/alpine-virt-console.iso ::/iso/test.iso
mcopy -i $S/esp.img tools/732/hype.cfg ::/hype.cfg

cp /usr/share/edk2/ovmf/OVMF_VARS.fd $S/VARS.fd
timeout "$SECS" "$QEMU" \
  -machine q35 -m 8192 -nodefaults \
  -accel kvm -cpu host,topoext=on -smp cpus=8,sockets=1,cores=4,threads=2 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=$S/VARS.fd \
  -drive format=raw,file=$S/esp.img,if=none,id=esp \
  -device ide-hd,drive=esp,bus=ide.0,bootindex=0 \
  -serial "file:$S/serial.log" -display none -vga std 2>$S/qemu.err || true

echo "=== what the config said"
grep -a "cfg: loaded\|cfg: autostart\|cfg:   autostart" $S/serial.log | head -8
echo "=== the boot decision per VM"
grep -a "#732: vm\|M9-4: vm" $S/serial.log | head -6
echo "=== which guests actually ran"
grep -ac "vm0 ttyS0|" $S/serial.log | sed 's/^/vm0 serial records: /'
grep -ac "vm1 ttyS0|" $S/serial.log | sed 's/^/vm1 serial records: /'

fail=0
say() { echo "$1"; fail=1; }
grep -aq "cfg: loaded" $S/serial.log || say "FAIL: the config never loaded -- nothing below means anything"
grep -aq "#732: vm1 'held' is not in \[hype\] autostart" $S/serial.log \
  || say "FAIL: 'held' was not held off by autostart [#732]"
grep -aq "autostart names 'nosuchvm', which is not a VM" $S/serial.log \
  || say "FAIL: the unmatched autostart name was not reported [#732]"
grep -aq "#732: vm0 'keep' is not in" $S/serial.log \
  && say "FAIL: 'keep' IS listed in autostart and was held off anyway [#732]"
# The held VM must produce no guest output at all; the started one must produce some.
v0=$(grep -ac "vm0 ttyS0|" $S/serial.log || true)
v1=$(grep -ac "vm1 ttyS0|" $S/serial.log || true)
[ "${v0:-0}" -gt 0 ] || say "FAIL: 'keep' was listed in autostart and never produced guest output"
[ "${v1:-0}" -eq 0 ] || say "FAIL: 'held' produced $v1 guest serial record(s) -- it ran anyway [#732]"
[ $fail -eq 0 ] && echo "ALL PASS: autostart gated the boot, the typo warned, the boot continued"
exit $fail
