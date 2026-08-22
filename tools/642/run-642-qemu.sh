#!/bin/bash
# #642: the periodic "screen|" dump must not replay a static screen forever.
#
# logchatter emits a fixed number of lines fast, then goes idle -- so its final
# line becomes static well before the run ends. The screen dump fires every 30s
# (RT-2c); running past two dump intervals with the guest already idle means a
# fixed screen must be dumped exactly ONCE, not once per interval.
set -e
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-disk-images/642}"
B=build
SECS="${1:-95}"
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
cmdline = lines=20
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

for ATTEMPT in 1 2 3; do
  cp "$VARS" $S/VARS.fd
  timeout "$SECS" qemu-system-x86_64 \
    -machine q35 -m 1024 -nodefaults \
    -accel kvm -accel tcg -cpu host -smp 2 \
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

echo "=== verdict ==="
rc=0
n=$(grep -ac "screen| micro/logchatter: emitted 20 lines" $S/serial.txt || true)
echo "'screen|' copies of the final (static) line: $n"
if [ "$n" -lt 1 ]; then
  echo "FAIL: the final line never appeared in a screen dump at all"
  rc=1
elif [ "$n" -gt 1 ]; then
  echo "FAIL: a static screen was dumped $n times -- it should be dumped exactly once"
  rc=1
else
  echo "PASS: static screen dumped exactly once despite the run spanning multiple 30s intervals"
fi
[ "$rc" -eq 0 ] && echo "PASS: #642 fixed"
exit "$rc"
