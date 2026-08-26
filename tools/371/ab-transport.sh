#!/bin/bash
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
# #371 round 4: does the failure survive when AHCI is removed from the path?
#
# The payload is eliminated (a 2.5 KB hello-world fails too) and the firmware BUILD is eliminated
# (two independent edk2 builds fail at the same rate). What remains is the transport: OVMF hangs on
# the first READ DMA of the boot disk and QEMU never completes it.
#
#   ahci    the ESP on q35's ICH9 AHCI controller -- the configuration every failure so far used,
#           and the one hype needs, because it reaches its host disk through its own AHCI driver.
#   virtio  the same ESP behind virtio-blk-pci. This removes BOTH suspects at once: QEMU's AHCI
#           device model and edk2's AtaAtapiPassThruDxe. OVMF drives it through VirtioBlkDxe.
#
# So this cannot say WHICH of the two owns the bug -- it says whether the AHCI stack owns it at all.
# A clean virtio arm alongside a failing AHCI arm confirms the subsystem; both failing would move
# the suspicion somewhere shared and much further up.
#
# The AHCI arm is a CONTROL RUN IN THE SAME SESSION, not a number quoted from an earlier batch: the
# observed rate has already moved between rounds (15% without debugcon, 6.7% with), so comparing
# today's virtio against yesterday's AHCI would prove nothing.
#
# No -debugcon: it makes the event ~2x rarer, and this comparison needs the conditions where it is
# common.
cd "$(dirname "$0")/../.."
N="${1:-20}"; SECS="${2:-22}"
ESP=disk-images/esp-371-hello.img
OUT="${OUT371:-/tmp/ab-transport-371}"
mkdir -p "$OUT"
ahci_ok=0; ahci_no=0; virt_ok=0; virt_no=0

run_one() {
  local arm="$1" i="$2"
  local log="$OUT/$arm-$i.log"
  cp -f /usr/share/edk2/ovmf/OVMF_VARS.fd "$OUT/$arm-$i.vars.fd"; rm -f "$log"
  if [ "$arm" = ahci ]; then
    set -- -drive format=raw,file="$ESP"
  else
    set -- -drive id=d0,if=none,format=raw,file="$ESP" -device virtio-blk-pci,drive=d0
  fi
  "$QEMU" -machine q35 -m 8192 -nodefaults -accel kvm -cpu host -smp 4 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
    -drive if=pflash,format=raw,file="$OUT/$arm-$i.vars.fd" \
    "$@" \
    -serial "file:$log" -display none -vga std 2>"$OUT/$arm-$i.stderr" &
  local qpid=$! t
  for t in $(seq "$SECS"); do kill -0 $qpid 2>/dev/null || break; sleep 1; done
  kill -9 $qpid 2>/dev/null; wait $qpid 2>/dev/null
  # killall, never pkill/pgrep: those match the truncated comm (qemu-system-x86) and silently no-op.
  killall -9 "$(basename "$QEMU")" 2>/dev/null; sleep 1
  local bytes hit
  bytes=$(wc -c < "$log")
  hit=$(LC_ALL=C grep -ac "hello: build" "$log" 2>/dev/null || echo 0)
  rm -f "$OUT/$arm-$i.vars.fd"
  if [ "$hit" -gt 0 ]; then echo "  $arm run $i: BOOTED bytes=$bytes"; return 0; fi
  echo "  $arm run $i: NOBOOT bytes=$bytes"; return 1
}

for i in $(seq 1 "$N"); do
  echo "pair $i:"
  if run_one ahci "$i"; then ahci_ok=$((ahci_ok+1)); else ahci_no=$((ahci_no+1)); fi
  if run_one virtio "$i"; then virt_ok=$((virt_ok+1)); else virt_no=$((virt_no+1)); fi
done

echo
echo "RESULT over $N pairs (no debugcon):"
echo "  ahci (ICH9 AHCI + AtaAtapiPassThruDxe) : $ahci_ok booted, $ahci_no noboot"
echo "  virtio-blk (VirtioBlkDxe)              : $virt_ok booted, $virt_no noboot"
