#!/bin/bash
# #751/#749/#735: run test code on a guest AP, and prove the AP absorbs an unclaimed MMIO
# access instead of spinning on it forever.
#
# This is the rig that closes #735's last gap. tools/749 proves the absorber -- but on the
# BSP, because every other microtest is single-vCPU. The bug was in the AP loop, and four
# attempts to drive it from a Linux guest all failed on guest tooling or on
# CONFIG_STRICT_DEVMEM (see tools/735/run-735-devmem.sh's header). So the guest brings up
# its own AP.
#
# PRE-FIX the AP spins on its first read, the markers never appear, and the guest reports
# MICRO FAIL with mark1=0 -- which names the failure precisely rather than timing out.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-disk-images/751}"
B=build
SECS="${1:-120}"
CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE.fd}
VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS.fd}
mkdir -p "$S"
killall -9 "$(basename "$QEMU")" 2>/dev/null || true; sleep 1

cat > $S/hype.cfg <<'CFG'
[hype]
config_version = 1
log_level = debug

[vm.apunclaimed]
label = apunclaimed
vcpus = 2
mem_mb = 512
boot = kernel
kernel = \EFI\hype\micro\apunclaimed.bin
os_hint = none
CFG

rm -f $S/esp.img
dd if=/dev/zero of=$S/esp.img bs=1M count=256 status=none
sfdisk -q $S/esp.img <<'PT'
label: gpt
start=2048, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B
PT
E="$S/esp.img@@1M"
mkfs.vfat -F 32 -n HYPEESP --offset 2048 $S/esp.img >/dev/null
mmd -i "$E" ::/EFI ::/EFI/BOOT ::/EFI/hype ::/EFI/hype/micro
mcopy -i "$E" $B/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$E" fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
# `make test` removes build/micro, so a rig run straight after one dies with a bare
# "build/micro/apunclaimed.bin: No such file or directory" -- which reads like a broken rig
# rather than a missing build step. It has cost two false failures. Build it here
# instead of depending on the caller's ordering.
[ -f $B/micro/apunclaimed.bin ] || make micro >/dev/null 2>&1 || \
  { echo "FAIL: could not build the microtests"; exit 1; }
mcopy -i "$E" $B/micro/apunclaimed.bin ::/EFI/hype/micro/apunclaimed.bin
mcopy -i "$E" $S/hype.cfg ::/hype.cfg

for ATTEMPT in 1 2 3; do
  cp "$VARS" $S/VARS.fd
  timeout "$SECS" "$QEMU" \
    -machine q35 -m 1024 -nodefaults \
    -accel kvm -accel tcg -cpu host -smp 4 \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file=$S/VARS.fd \
    -drive format=raw,file=$S/esp.img,if=none,id=esp \
    -device ide-hd,drive=esp,bus=ide.0,bootindex=0 \
    -serial stdio -display none -vga std > $S/serial-$ATTEMPT.txt 2>&1 || true
  if grep -aq "hype: build" $S/serial-$ATTEMPT.txt; then
    echo "attempt $ATTEMPT: hype ran"; cp $S/serial-$ATTEMPT.txt $S/serial.txt; break
  fi
  echo "attempt $ATTEMPT: hype never ran (#371 noboot) -- retrying"
done

echo "=== what the guest said"
grep -a "micro/apunclaimed" $S/serial.txt | head -6
echo "=== what hype said about the unclaimed accesses"
grep -a "claimed by NO device\|UNDECODABLE" $S/serial.txt | head -4
grep -a "APVCPU vm0/1" $S/serial.txt | tail -1 | sed -n 's/.*\(exits=[0-9]* unhandled=[0-9]* unclaimed=[0-9]*\).*/  vCPU1 \1/p'

echo "=== verdict ==="
rc=0
if ! grep -aq "hype: build" $S/serial.txt; then echo "NOBOOT: hype never ran (#371)"; exit 2; fi
if ! grep -aq "micro/apunclaimed: bringing up vCPU 1" $S/serial.txt; then
  echo "FAIL: the guest never started -- nothing to conclude"; exit 1
fi
if grep -aq "MICRO PASS: apunclaimed" $S/serial.txt; then
  echo "PASS: a guest AP absorbed two unclaimed accesses and kept running [#751 #749 #735]"
elif grep -aq "MICRO FAIL: apunclaimed" $S/serial.txt; then
  echo "FAIL: the guest ran but the access behaved wrongly -- see its own line above"; rc=1
else
  # No verdict at all is the PRE-FIX symptom, and worth naming as such rather than as a
  # generic timeout: the vCPU is spinning on the first read and will never print again.
  echo "FAIL: no verdict -- the AP never got past its first unclaimed read."
  echo "      That is exactly the #735 wedge: RIP never advances past the instruction."
  rc=1
fi
# The unhandled counter must NOT be the one climbing: an access that is completed is not
# unhandled, and conflating them is what made this invisible for five hardware boots.
if grep -aq "UNDECODABLE" $S/serial.txt; then
  echo "NOTE: at least one access could not be decoded and was left alone -- see the line above"
fi
# THE assertion: an AP absorbed unclaimed accesses and none were left unhandled.
uc=$(grep -a "APVCPU vm0/1" $S/serial.txt | tail -1 | sed -n 's/.*unclaimed=\([0-9]*\).*/\1/p')
uh=$(grep -a "APVCPU vm0/1" $S/serial.txt | tail -1 | sed -n 's/.*unhandled=\([0-9]*\).*/\1/p')
echo "vCPU 1: unclaimed=${uc:-?} unhandled=${uh:-?}   (5950X boot 6, pre-fix: unhandled=37009095)"
[ "${uc:-0}" -ge 2 ] 2>/dev/null || { echo "FAIL: the AP absorbed fewer than 2 -- path unproven"; rc=1; }
[ "${uh:-1}" -eq 0 ] 2>/dev/null || { echo "FAIL: the AP still has unhandled NPFs"; rc=1; }
exit "$rc"
