#!/bin/bash
# #749/#735: an access to an address NO device claims must complete, not wedge the vCPU.
#
# This is the rig for the fault that stopped #735's reboot. On the 5950X, boot 6, vCPU 1
# took 37,009,095 nested page faults at one RIP -- ~350,000/s from t=170s to the end of the
# run -- because hype counted an unmatched NPF and re-entered with RIP unchanged, so the
# guest re-executed the same instruction forever. The operator's `reboot`, pinned to that
# vCPU, could never run.
#
# The guest is tests/micro/unclaimed.c, which reads 0x100000004 -- the exact address the
# 5950X wedged on. Pre-fix, the FIRST read never returns and this rig times out with no
# verdict, which is the honest pre-fix symptom. Post-fix it does a thousand of them.
#
# Two vCPUs, because the fault was on an AP and the AP loop is the path that was wrong; the
# BSP loop has always reported an unmodelled access rather than resuming blind.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-disk-images/749}"
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

[vm.unclaimed]
label = unclaimed
vcpus = 2
mem_mb = 512
boot = kernel
kernel = \EFI\hype\micro\unclaimed.bin
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
mcopy -i "$E" $B/micro/unclaimed.bin ::/EFI/hype/micro/unclaimed.bin
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
grep -a "micro/unclaimed" $S/serial.txt | head -6
echo "=== what hype said about the unclaimed accesses"
grep -a "claimed by NO device\|UNDECODABLE" $S/serial.txt | head -4
grep -a "APVCPU" $S/serial.txt | tail -1 | sed -n 's/.*\(unhandled=[0-9]* unclaimed=[0-9]*\).*/  \1/p'

echo "=== verdict ==="
rc=0
if ! grep -aq "hype: build" $S/serial.txt; then echo "NOBOOT: hype never ran (#371)"; exit 2; fi
if ! grep -aq "micro/unclaimed: reading an address" $S/serial.txt; then
  echo "FAIL: the guest never started -- nothing to conclude"; exit 1
fi
if grep -aq "MICRO PASS: unclaimed" $S/serial.txt; then
  echo "PASS: 1000 unclaimed accesses completed and execution continued [#749]"
elif grep -aq "MICRO FAIL: unclaimed" $S/serial.txt; then
  echo "FAIL: the guest ran but the access behaved wrongly -- see its own line above"; rc=1
else
  # No verdict at all is the PRE-FIX symptom, and worth naming as such rather than as a
  # generic timeout: the vCPU is spinning on the first read and will never print again.
  echo "FAIL: no verdict -- the guest never got past its first unclaimed read."
  echo "      That is exactly the #735 wedge: RIP never advances past the instruction."
  rc=1
fi
# The unhandled counter must NOT be the one climbing: an access that is completed is not
# unhandled, and conflating them is what made this invisible for five hardware boots.
if grep -aq "UNDECODABLE" $S/serial.txt; then
  echo "NOTE: at least one access could not be decoded and was left alone -- see the line above"
fi
exit "$rc"
