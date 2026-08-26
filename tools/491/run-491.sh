#!/bin/bash
# TERM-15 (#491) leg. Boot 1: two VMs; QMP types `delete beta` + the two confirmations at the
# dashboard. Boot 2: the SAME ESP again, unstaged -- the config write must survive, so hype
# comes up with ONE VM and no [vm.beta] anywhere.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
cd "$(git rev-parse --show-toplevel)"
killall -9 "$(basename "$QEMU")" 2>/dev/null || true
sleep 1
KEYS=$(cat tools/491/sendkeys.txt)
HYPE_CFG=tools/491/hype.cfg SENDKEYS="$KEYS" \
    tools/run-guest.sh disk-images/alpine-hype-dbg.iso 491-delete "${TIMEOUT:-240}" || true
LOG=disk-images/run-491-delete.log
ESP=disk-images/run-491-delete.esp.img

echo "=== boot 1: the delete ==="
LC_ALL=C grep -a "TERMCMD\|DELETE" "$LOG" | head -8
LC_ALL=C grep -aq "DELETE: vm1 'beta' removed from hype.cfg" "$LOG" || { echo "FAIL: no removal line"; exit 1; }

echo "=== the written config (from the ESP, host-side) ==="
mcopy -i "$ESP@@1M" ::/hype.cfg /tmp/claude-1000/hype-491-after.cfg -n
grep -c "vm.beta" /tmp/claude-1000/hype-491-after.cfg && { echo "FAIL: [vm.beta] still in hype.cfg"; exit 1; }
grep -q "\[vm.alpha\]" /tmp/claude-1000/hype-491-after.cfg || { echo "FAIL: alpha vanished too"; exit 1; }
echo "hype.cfg on disk holds alpha only"

echo "=== boot 2: same ESP, no restage ==="
killall -9 "$(basename "$QEMU")" 2>/dev/null || true
# HOST vars to pair with the HOST code blob -- fw/OVMF_VARS.fd is the GUEST-side varstore and
# the mixed pair boots to a silent 0-byte log (the known mixup).
cp /usr/share/edk2/ovmf/OVMF_VARS.fd /tmp/claude-1000/hype-491-vars.fd
timeout 150 "$QEMU" -machine q35 -m 8192 -nodefaults \
  -accel kvm -cpu host -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/claude-1000/hype-491-vars.fd \
  -device ich9-ahci,id=ahci \
  -drive format=raw,file="$ESP",if=none,id=d0 \
  -device ide-hd,drive=d0,bus=ahci.0,bootindex=0 \
  -serial file:/tmp/claude-1000/hype-491-boot2.log -display none -vga none || true
echo "=== boot 2 verdict ==="
LC_ALL=C grep -a "cfg: loaded\|cfg:   vm\[" /tmp/claude-1000/hype-491-boot2.log | head -4
LC_ALL=C grep -aq -- "-- 1 VM(s)" /tmp/claude-1000/hype-491-boot2.log || { echo "FAIL: boot 2 did not load exactly 1 VM"; exit 1; }
LC_ALL=C grep -aq "beta" /tmp/claude-1000/hype-491-boot2.log && { echo "FAIL: beta came back"; exit 1; }
echo "PASS: beta deleted, written through, and gone after a power cycle"
