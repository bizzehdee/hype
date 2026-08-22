#!/bin/bash
# #643: boot the SAME media twice without wiping it between boots, and confirm
# both boots' logs survive -- HYPE.LOG (boot 2's fresh log) plus HYPE.1.LOG
# (boot 1's log, rotated out of the way instead of truncated), each carrying a
# distinct #643 boot-counter line, correctly ordered.
set -e
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-disk-images/643}"
B=build
SECS="${1:-60}"
CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE.fd}
VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS.fd}
mkdir -p "$S"
killall -9 qemu-system-x86_64 2>/dev/null || true; sleep 1

cat > $S/hype.cfg <<'CFG'
[hype]
config_version = 1
log_level = debug

[vm.chatter]
label = logchatter
vcpus = 1
mem_mb = 512
boot = kernel
kernel = \EFI\hype\micro\logchatter.bin
cmdline = lines=5
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
mcopy -i "$E" $B/micro/logchatter.bin ::/EFI/hype/micro/logchatter.bin
mcopy -i "$E" $S/hype.cfg ::/hype.cfg
cp "$VARS" $S/VARS.fd

run_once() {
  local attempt_log="$1"
  for ATTEMPT in 1 2 3; do
    timeout "$SECS" qemu-system-x86_64 \
      -machine q35 -m 1024 -nodefaults \
      -accel kvm -accel tcg -cpu host -smp 2 \
      -drive if=pflash,format=raw,readonly=on,file="$CODE" \
      -drive if=pflash,format=raw,file=$S/VARS.fd \
      -device qemu-xhci,id=xhci \
      -drive format=raw,file=$S/esp.img,if=none,id=espstick \
      -device usb-storage,bus=xhci.0,drive=espstick,port=1,bootindex=0 \
      -serial stdio -display none -vga std > "$attempt_log" 2>&1 || true
    if grep -aq "hype: build" "$attempt_log"; then return 0; fi
    echo "  attempt $ATTEMPT: hype never ran (#371 noboot) -- retrying"
  done
  return 1
}

echo "=== boot 1 (fresh media -- no prior HYPE.LOG to rotate) ==="
run_once $S/serial-boot1.txt

echo "=== mounting ESP to inspect between boots (udisks loop-mount, per this repo's recipe) ==="
LOOPDEV=$(udisksctl loop-setup -f $S/esp.img --no-user-interaction | grep -o '/dev/loop[0-9]*')
udisksctl mount -b "${LOOPDEV}p1" --no-user-interaction
MNT=$(udisksctl info -b "${LOOPDEV}p1" | grep MountPoints | awk '{print $2}')
ls -la "$MNT" | grep -i 'HYPE'
udisksctl unmount -b "${LOOPDEV}p1" --no-user-interaction
udisksctl loop-delete -b "$LOOPDEV" --no-user-interaction

echo "=== boot 2 (same media, VARS.fd carried over unchanged -- must rotate boot 1's log) ==="
run_once $S/serial-boot2.txt

echo "=== mounting ESP for the verdict (udisks loop-mount) ==="
LOOPDEV=$(udisksctl loop-setup -f $S/esp.img --no-user-interaction | grep -o '/dev/loop[0-9]*')
udisksctl mount -b "${LOOPDEV}p1" --no-user-interaction
MNT=$(udisksctl info -b "${LOOPDEV}p1" | grep MountPoints | awk '{print $2}')
ls -la "$MNT" | grep -i 'HYPE'
rc=0
if [ -f "$MNT/HYPE.LOG" ] && [ -f "$MNT/HYPE.1.LOG" ]; then
  echo "PASS: both HYPE.LOG (boot 2) and HYPE.1.LOG (rotated boot 1) present"
else
  echo "FAIL: expected both HYPE.LOG and HYPE.1.LOG to exist after two boots"
  rc=1
fi
n1=$(LC_ALL=C grep -a "boot #" "$MNT/HYPE.1.LOG" 2>/dev/null | head -1 | grep -oE 'boot #[0-9]+' | grep -oE '[0-9]+')
n2=$(LC_ALL=C grep -a "boot #" "$MNT/HYPE.LOG" 2>/dev/null | head -1 | grep -oE 'boot #[0-9]+' | grep -oE '[0-9]+')
echo "HYPE.1.LOG boot counter: ${n1:-MISSING}; HYPE.LOG boot counter: ${n2:-MISSING}"
if [ -n "$n1" ] && [ -n "$n2" ] && [ "$n2" -eq "$((n1 + 1))" ]; then
  echo "PASS: boot counters are present, distinct, and correctly ordered ($n1 -> $n2)"
else
  echo "FAIL: boot counters missing or not sequential"
  rc=1
fi
if grep -aq "logchatter" "$MNT/HYPE.1.LOG" 2>/dev/null; then
  echo "PASS: boot 1's actual content (not just its banner) survived in HYPE.1.LOG"
else
  echo "FAIL: boot 1's content did not survive rotation"
  rc=1
fi
udisksctl unmount -b "${LOOPDEV}p1" --no-user-interaction
udisksctl loop-delete -b "$LOOPDEV" --no-user-interaction
[ "$rc" -eq 0 ] && echo "PASS: #643 two-boot rotation works end to end"
exit "$rc"
