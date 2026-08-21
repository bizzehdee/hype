#!/bin/bash
# #639: prove a guest's console output reaches hype's log INTACT, and that an
# input script gating on the shell prompt therefore completes.
#
# The bar comes straight from the boot-D failure: hype dropped 5941 of 41199
# characters of a guest's console in one burst at login, the shell prompt was
# inside the dropped burst, and `expect ~#` could never match -- a healthy
# guest at a live prompt looked wedged for 17 minutes. So this rig asserts
# three things about one 2-vCPU Alpine guest driven by a script:
#   1. UARTTX reports dropped=0 on every port (loss, as opposed to stalled,
#      which is back-pressure and costs nothing);
#   2. the prompt actually appears in the guest log;
#   3. the script reaches its pass marker.
# A run that stalls (nonzero stalled, dropped=0) still passes: the guest waited,
# nothing was lost. That is the whole point of the fix.
set -e
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-disk-images/639}"
B=build
SECS="${1:-600}"
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
label = uart639
vcpus = 2
mem_mb = 1024
boot = installer
install_media = \iso\test.iso
firmware = uefi
os_hint = linux
target_disk = file:\hype\disks\smp.img
CFG

# The script writes ~4 KB of console output in one burst right after login --
# more than the old 256-byte ring could hold and enough to exercise
# back-pressure on the 4 KiB one -- and only then checks for the prompt again.
# If any of that burst is lost, the final expect cannot match.
cat > $S/vm0.txt <<'IN'
timeout 700000
fail-if soft lockup

expect localhost login:
send root\n
expect ~#
send for i in $(seq 1 60); do echo "639-BURST-$i-0123456789012345678901234567890123456789012345678901234"; done\n
expect 639-BURST-60
send echo "U639" "INTACT"\n
expect U639 INTACT
pass uart-no-loss
IN

# GPT, not superfloppy: a bare-FAT image kills the media resolver (#525).
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
last=$(grep -a "UARTTX:" $S/serial.txt | tail -1 || true)
echo "${last:-UARTTX: (no sample -- the run did not get far enough)}"
if [ -z "$last" ]; then
  echo "FAIL: no UARTTX sample in the log"; rc=1
elif echo "$last" | grep -aq "dropped=[1-9]"; then
  echo "FAIL: console output was LOST -- a prompt or a line is missing from this log"; rc=1
else
  echo "no console loss: every dropped= is 0"
fi
if grep -aq "639-BURST-60" $S/serial.txt; then
  echo "burst arrived intact (last line of 60 present)"
else
  echo "FAIL: the 4 KB burst did not arrive whole"; rc=1
fi
if grep -aq "uart-no-loss" $S/serial.txt; then
  echo "input script: PASS (prompt seen after the burst)"
else
  echo "FAIL: script pass marker absent -- expect never matched a prompt"; rc=1
fi
[ "$rc" -eq 0 ] && echo "PASS: guest console intact, script completed"
exit "$rc"
