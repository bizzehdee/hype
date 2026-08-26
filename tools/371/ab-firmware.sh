#!/bin/bash
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
# #371 round 3: is the bannerless boot the FIRMWARE BUILD's fault, or QEMU's?
#
# Same minimal payload, same ESP, same QEMU command line. The only variable is which OVMF builds
# the machine boots:
#
#   fedora  /usr/share/edk2/ovmf/OVMF_CODE.fd  -- edk2-ovmf-20260508-8.fc44, a DOWNSTREAM distro
#           build (release -8 implies Fedora patches). This is the one all failures so far used.
#   vendor  fw/OVMF_CODE.fd                    -- the independent edk2 build this repo vendors as
#           GUEST firmware. Different build entirely, and not involved in any failure so far.
#
# CODE is paired with its own VARS in both arms. Mixing a 2MB CODE with a 4MB VARS produces a
# 0-byte log that looks exactly like the failure under investigation -- the one trap that would
# invalidate this whole experiment.
#
# NO -debugcon here, deliberately. Round 2 ran with it and saw 1 failure in 25 (4%) against 15% in
# round 1 without it: 252 KB of debug output per boot changes the timing enough to suppress the
# event. Instrumenting a race out of existence is a real hazard, so this arm-to-arm comparison runs
# under the conditions where the failure is common.
cd /mnt/data/dev/hype
N="${1:-20}"; SECS="${2:-22}"
ESP=disk-images/esp-371-hello.img
OUT=/tmp/claude-1000/-mnt-data-dev-hype/856105a5-c1a1-4cdd-b15f-f7cce249ea7b/scratchpad/fw371
mkdir -p "$OUT"

fed_ok=0; fed_no=0; ven_ok=0; ven_no=0

run_one() {
  local arm="$1" code="$2" vars="$3" i="$4"
  local log="$OUT/$arm-$i.log"
  cp -f "$vars" "$OUT/$arm-$i.vars.fd"; rm -f "$log"
  "$QEMU" -machine q35 -m 8192 -nodefaults -accel kvm -cpu host -smp 4 \
    -drive if=pflash,format=raw,readonly=on,file="$code" \
    -drive if=pflash,format=raw,file="$OUT/$arm-$i.vars.fd" \
    -drive format=raw,file="$ESP" \
    -serial "file:$log" -display none -vga std 2>"$OUT/$arm-$i.stderr" &
  local qpid=$! t
  for t in $(seq "$SECS"); do kill -0 $qpid 2>/dev/null || break; sleep 1; done
  kill -9 $qpid 2>/dev/null; wait $qpid 2>/dev/null
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
  if run_one fedora /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/edk2/ovmf/OVMF_VARS.fd "$i"; then
    fed_ok=$((fed_ok+1)); else fed_no=$((fed_no+1)); fi
  if run_one vendor fw/OVMF_CODE.fd fw/OVMF_VARS.fd "$i"; then
    ven_ok=$((ven_ok+1)); else ven_no=$((ven_no+1)); fi
done

echo
echo "RESULT over $N pairs (no debugcon):"
echo "  fedora edk2-20260508-8.fc44 : $fed_ok booted, $fed_no noboot"
echo "  repo-vendored fw/OVMF       : $ven_ok booted, $ven_no noboot"
