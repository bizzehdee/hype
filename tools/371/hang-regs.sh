#!/bin/bash
# #371 round 5: at the moment of the hang, ask QEMU what the AHCI port registers say.
#
# THE DISCRIMINATOR THE TICKET HAS BEEN MISSING.
#
# edk2's AtaAtapiPassThruDxe completes a sync DMA by POLLING PxIS for the DHRS bit
# (AhciCheckFisReceived) -- not PxCI, and not an interrupt. AhciStartCommand clears PxIS *before*
# writing PxCI, so a completion cannot be lost by the driver clearing it afterwards. That leaves
# three distinguishable states at hang time:
#
#   PxCI bit0 SET,   PxTFD.BSY set    -> QEMU never completed the command       -> QEMU
#   PxCI bit0 clear, PxIS.DHRS clear  -> QEMU retired it WITHOUT raising DHRS   -> QEMU
#   PxCI bit0 clear, PxIS.DHRS SET    -> QEMU signalled it, driver missed it    -> edk2
#
# Healthy idle baseline on this host, measured on a booted guest:
#   PxIS=0x00000001 (DHRS sticky from the last command), PxTFD=0x00000050 (DRDY, !BSY), PxCI=0
set -u
cd "$(dirname "$0")/../.."
N="${1:-40}"; SECS="${2:-25}"
ESP=disk-images/esp-371-hello.img
OUT="${OUT371:-rig/371r5}"
DBG="${DEBUGCON:-1}"      # DEBUGCON=0 keeps the failure ~2x more likely
mkdir -p "$OUT"
ok=0; no=0

hmp() { printf '%s\n' "$2" | timeout 5 socat - "UNIX-CONNECT:$1" 2>/dev/null | tr -d '\r' \
        | sed 's/\x1b\[[0-9;]*[A-Za-z]//g; s/.*\x08//' | grep -E "^0000|^[0-9A-Z]+ =" ; }
rd() { printf 'xp/1xw %s\n' "$2" | timeout 5 socat - "UNIX-CONNECT:$1" 2>/dev/null \
       | tr -d '\r' | grep -oE "0x[0-9a-f]{8}" | tail -1; }

for i in $(seq 1 "$N"); do
  log="$OUT/run$i.log"; dbg="$OUT/run$i.debugcon"; mon="$OUT/run$i.mon"
  rm -f "$log" "$dbg" "$mon"; cp -f /usr/share/edk2/ovmf/OVMF_VARS.fd "$OUT/run$i.vars.fd"
  dbgargs=(); [ "$DBG" = 1 ] && dbgargs=(-debugcon "file:$dbg" -global isa-debugcon.iobase=0x402)
  qemu-system-x86_64 -machine q35 -m 8192 -nodefaults -accel kvm -cpu host -smp 4 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
    -drive if=pflash,format=raw,file="$OUT/run$i.vars.fd" \
    -drive format=raw,file="$ESP" "${dbgargs[@]}" \
    -monitor "unix:$mon,server=on,wait=off" \
    -serial "file:$log" -display none -vga std 2>"$OUT/run$i.stderr" &
  qpid=$!; hit=0; at=0
  for t in $(seq "$SECS"); do
    kill -0 $qpid 2>/dev/null || break
    LC_ALL=C grep -aq "hello: build" "$log" 2>/dev/null && { hit=1; at=$t; break; }
    sleep 1
  done
  if [ "$hit" = 1 ]; then
    ok=$((ok+1))
    # A banner that arrives LATE is the whole question: the boot-path ATA read has a 31 s timeout
    # (AtaPassThruExecute.c: EFI_TIMER_PERIOD_SECONDS(0 + 31) for a 1-sector DMA read), and every
    # harness before this one killed QEMU at ~22-25 s -- before it could expire. So a "noboot" may
    # only ever have been a stall nobody waited out.
    if [ "$at" -gt 15 ]; then echo "run $i: BOOTED **LATE** at ${at}s <<<<<"; else echo "run $i: BOOTED at ${at}s"; fi
  else
    no=$((no+1))
    echo "run $i: ===== NOBOOT (serial $(wc -c < "$log") bytes) -- interrogating QEMU ====="
    hmp "$mon" "info pci" > "$OUT/run$i.pci.txt" 2>/dev/null
    printf 'info pci\n' | timeout 5 socat - "UNIX-CONNECT:$mon" 2>/dev/null | tr -d '\r' > "$OUT/run$i.pci.txt"
    abar=$(grep -A6 -i "SATA controller" "$OUT/run$i.pci.txt" | grep -oiE "BAR5: 32 bit memory at 0x[0-9a-f]+" | grep -oE "0x[0-9a-f]+" | head -1)
    echo "    ABAR=${abar:-UNKNOWN}"
    if [ -n "$abar" ]; then
      p=$((abar+0x100))
      is=$(rd  "$mon" "$(printf '0x%x' $((p+0x10)))")
      tfd=$(rd "$mon" "$(printf '0x%x' $((p+0x20)))")
      ci=$(rd  "$mon" "$(printf '0x%x' $((p+0x38)))")
      cmd=$(rd "$mon" "$(printf '0x%x' $((p+0x18)))")
      serr=$(rd "$mon" "$(printf '0x%x' $((p+0x30)))")
      ssts=$(rd "$mon" "$(printf '0x%x' $((p+0x28)))")
      ghc=$(rd "$mon" "$(printf '0x%x' $((abar+0x04)))")
      gis=$(rd "$mon" "$(printf '0x%x' $((abar+0x08)))")
      echo "    PxIS=$is  PxTFD=$tfd  PxCI=$ci  PxCMD=$cmd  PxSERR=$serr  PxSSTS=$ssts  GHC=$ghc  IS=$gis"
      echo "$is $tfd $ci $cmd $serr $ssts $ghc $gis" > "$OUT/run$i.regs.txt"
    fi
    printf 'info registers\n' | timeout 5 socat - "UNIX-CONNECT:$mon" 2>/dev/null | tr -d '\r' \
      | grep -oE "RIP=[0-9a-f]+" | head -1 | sed 's/^/    /'
    if [ "$DBG" = 1 ]; then
      echo "    debugcon=$(wc -c < "$dbg" 2>/dev/null || echo 0) bytes; tail:"
      LC_ALL=C tail -c 300 "$dbg" 2>/dev/null | tr -d '\r' | sed 's/^/      dbg| /' | tail -10
    fi
  fi
  kill -9 $qpid 2>/dev/null; wait $qpid 2>/dev/null
  killall -9 qemu-system-x86_64 2>/dev/null; sleep 1
  rm -f "$OUT/run$i.vars.fd"
  [ "$hit" = 1 ] && rm -f "$dbg" "$log" "$OUT/run$i.stderr"
done
echo; echo "RESULT: $ok booted, $no noboot of $N (debugcon=$DBG)"
