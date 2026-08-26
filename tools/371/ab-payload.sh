#!/bin/bash
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
# #371 A/B: does a minimal EFI app fail to boot at the same rate hype does?
#
# Same base ESP image (one copy, then BOOTX64.EFI overwritten), same QEMU command line, same
# firmware, same varstore handling. The ONLY difference between the two arms is which PE the
# firmware loads. If hello fails at hype's rate, the fault is in QEMU/OVMF and hype is a bystander;
# if hello never fails, the fault is hype's and it is dying before its first serial write.
#
# INTERLEAVED, not batched: host state drifts (page cache, thermals, KVM state), and running 20 of
# one arm then 20 of the other would let that drift masquerade as a difference between the arms.
cd /mnt/data/dev/hype
N="${1:-20}"; SECS="${2:-25}"
hello_ok=0; hello_no=0; hype_ok=0; hype_no=0

run_one() {
  local arm="$1" esp="$2" banner="$3" i="$4"
  local log="disk-images/ab371-$arm-$i.log"
  cp -f /usr/share/edk2/ovmf/OVMF_VARS.fd "$log.vars.fd"; rm -f "$log"
  "$QEMU" -machine q35 -m 8192 -nodefaults -accel kvm -cpu host -smp 4 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
    -drive if=pflash,format=raw,file="$log.vars.fd" \
    -drive format=raw,file="$esp" \
    -serial "file:$log" -display none -vga std 2>/dev/null &
  local qpid=$!
  local t
  for t in $(seq "$SECS"); do kill -0 $qpid 2>/dev/null || break; sleep 1; done
  kill -9 $qpid 2>/dev/null; wait $qpid 2>/dev/null
  # Never leave one behind -- see the pgrep/comm truncation trap; killall is the only reliable form.
  killall -9 "$(basename "$QEMU")" 2>/dev/null; sleep 1

  local bytes hit
  bytes=$(wc -c < "$log")
  hit=$(LC_ALL=C grep -ac "$banner" "$log")
  if [ "$hit" -gt 0 ]; then
    echo "  $arm run $i: BOOTED bytes=$bytes"
    return 0
  fi
  echo "  $arm run $i: NOBOOT bytes=$bytes"
  return 1
}

for i in $(seq 1 "$N"); do
  echo "pair $i:"
  if run_one hello disk-images/esp-371-hello.img "hello: build" "$i"; then
    hello_ok=$((hello_ok+1)); else hello_no=$((hello_no+1)); fi
  if run_one hype disk-images/esp-371-hype.img "hype: build" "$i"; then
    hype_ok=$((hype_ok+1)); else hype_no=$((hype_no+1)); fi
done

echo
echo "RESULT over $N pairs:"
echo "  hello.efi : $hello_ok booted, $hello_no noboot"
echo "  hype.efi  : $hype_ok booted, $hype_no noboot"
