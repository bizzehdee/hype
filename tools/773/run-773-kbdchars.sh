#!/bin/bash
# #773 instrument check: does KBDCHARS record what hype hands a guest?
#
# Two attempts to measure #773 off the guest's own echo have failed -- boot 36's keystrokes
# went to hype's terminal because nothing had attached the console, and boot 37's reached the
# guest correctly (routed=2420 of 2471) but RUN1A.LOG captured only the heartbeat and not one
# character of the typing. KBDCHARS records the characters at the point hype hands them over,
# where nothing downstream can lose them.
#
# An instrument nobody has seen fire is not an instrument. This rig focuses a guest with the
# leader chord, types a known string, and asserts the string comes back out of KBDCHARS.
#
# PASS = KBDCHARS present AND carrying the exact string that was typed.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-rig/i773}"
rm -rf "$S"; mkdir -p "$S"
killall -9 "$(basename "$QEMU")" 2>/dev/null || true
sleep 1

TYPED="abcdefghijklmnopqrstuvwxyz"

ISOB=$(ls disk-images/alpine-hype-dbg.iso)
SZ=$(( $(stat -c%s "$ISOB") / 1048576 + 160 ))
dd if=/dev/zero of="$S"/esp.img bs=1M count=$SZ conv=fsync status=none
sfdisk --label gpt -q "$S"/esp.img <<SFDISK
2048,,U
SFDISK
mformat -i "$S"/esp.img@@1M -F ::
mmd -i "$S"/esp.img@@1M ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso
make all >/dev/null 2>&1
mcopy -i "$S"/esp.img@@1M build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$S"/esp.img@@1M fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i "$S"/esp.img@@1M "$ISOB" ::/iso/test.iso
cat > "$S"/hype.cfg <<CFG
[hype]
config_version = 1
log_level = debug

[vm.u]
vcpus = 1
mem_mb = 1024
boot = installer
install_media = \iso\test.iso
firmware = uefi
os_hint = linux
net_mode = none
target_disk = file:\hype\disks\u.img
CFG
mcopy -i "$S"/esp.img@@1M "$S"/hype.cfg ::/hype.cfg

cp /usr/share/edk2/ovmf/OVMF_VARS.fd "$S"/VARS.fd
{
  # Wait for the guest to be up before focusing it -- an unready VM cannot be focused.
  for _ in $(seq 1 240); do
    sleep 1
    grep -aq "localhost login:" "$S"/serial.log 2>/dev/null && break
  done
  sleep 5
  # Focus vm0: the leader is Right-Ctrl + Right-Alt held, plus '1'.
  printf 'sendkey ctrl_r-alt_r-1\n'; sleep 2
  # Then type the known string, one key at a time.
  for c in $(echo "$TYPED" | fold -w1); do printf 'sendkey %s\n' "$c"; sleep 0.12; done
  sleep 25
  printf 'quit\n'
} | timeout "${1:-420}" "$QEMU" -machine q35 -m 4096 -nodefaults \
  -accel kvm -cpu host -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file="$S"/VARS.fd \
  -device ich9-ahci,id=ahci \
  -drive format=raw,file="$S"/esp.img,if=none,id=d0 \
  -device ide-hd,drive=d0,bus=ahci.0,bootindex=0 \
  -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 \
  -serial "file:$S/serial.log" -monitor stdio -display none -vga std >"$S"/mon.log 2>"$S"/qemu.err || true

echo "=== the chord was seen ==="
grep -a -o -E "host-kbd scancodes=[0-9]+ chords=[0-9]+" "$S"/serial.log | tail -1
grep -a -o -E "GUESTKBD vm0: routed=[0-9]+" "$S"/serial.log | tail -1

echo "=== KBDCHARS ==="
grep -a -o "KBDCHARS[^]]*\]" "$S"/serial.log | tail -2

grep -aq "KBDCHARS" "$S"/serial.log || {
  echo "FAIL: KBDCHARS never printed -- the instrument does not fire [#773]"; exit 1; }
grep -a "KBDCHARS" "$S"/serial.log | grep -aq "$TYPED" || {
  echo "FAIL: KBDCHARS printed but does not contain the typed string '$TYPED' [#773]"; exit 1; }
echo "ALL PASS: KBDCHARS carries exactly what was typed [#773]"
