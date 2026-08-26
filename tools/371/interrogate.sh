#!/bin/bash
# #371 round 7: catch the hang and interrogate it properly.
#
# Round 6 established two things and failed at a third:
#   * the hang is PERMANENT -- 0 of 15 failures recovered within 120 s, far past the 31 s timeout
#     the boot-path ATA read carries (AtaPassThruExecute.c, EFI_TIMER_PERIOD_SECONDS(0 + 31)).
#   * with -debugcon the rate is ~50%, not the ~7% seen without it. Instrumentation makes it MORE
#     likely here, the opposite of what round 3 recorded.
#   * the monitor never answered, because socat closed the connection before QEMU replied.
#
# This version holds the connection open. What it is after: WHICH module the deterministic RIP is
# in. The suspect is InternalAcpiDelay() in OvmfPkg/Library/AcpiTimerLib -- an unbounded
#   while (((Ticks - InternalAcpiGetTimerTick()) & BIT23) == 0) CpuPause();
# with no timeout, called from AhciWaitUntilFisReceived()'s MicroSecondDelay(100). If the ACPI PM
# timer stops advancing, that loop never exits, the AHCI poll never runs again, and the ATA timeout
# never counts down -- which matches every observation including the permanence.
set -u
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
cd "$(dirname "$0")/../.."
N="${1:-8}"; SECS="${2:-40}"; DBG="${DEBUGCON:-1}"
ESP=disk-images/esp-371-hello.img
OUT="${OUT371:-rig/371r7}"
mkdir -p "$OUT"
ok=0; no=0

ask() { { printf '%s\n' "$2"; sleep "${3:-1.5}"; } | socat -t4 - "UNIX-CONNECT:$1" 2>/dev/null \
        | tr -d '\r' | sed 's/\x1b\[[0-9;]*[A-Za-z]//g'; }

for i in $(seq 1 "$N"); do
  log="$OUT/run$i.log"; dbg="$OUT/run$i.debugcon"; mon="$PWD/$OUT/run$i.mon"
  rm -f "$log" "$dbg" "$mon"; cp -f /usr/share/edk2/ovmf/OVMF_VARS.fd "$OUT/run$i.vars.fd"
  dbgargs=(); [ "$DBG" = 1 ] && dbgargs=(-debugcon "file:$dbg" -global isa-debugcon.iobase=0x402)
  "$QEMU" -machine q35 -m 8192 -nodefaults -accel kvm -cpu host -smp 4 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
    -drive if=pflash,format=raw,file="$OUT/run$i.vars.fd" \
    -drive format=raw,file="$ESP" "${dbgargs[@]}" \
    -monitor "unix:$mon,server=on,wait=off" \
    -serial "file:$log" -display none -vga std 2>"$OUT/run$i.stderr" &
  qpid=$!; hit=0
  for t in $(seq "$SECS"); do
    kill -0 $qpid 2>/dev/null || break
    LC_ALL=C grep -aq "hello: build" "$log" 2>/dev/null && { hit=1; break; }
    sleep 1
  done

  if [ "$hit" = 1 ]; then
    ok=$((ok+1)); echo "run $i: BOOTED"
    rm -f "$dbg" "$log" "$OUT/run$i.stderr"
  else
    no=$((no+1))
    if kill -0 $qpid 2>/dev/null; then alive="QEMU ALIVE"; else alive="QEMU DEAD (crashed)"; fi
    echo "run $i: ===== NOBOOT ($(wc -c < "$log") byte serial) -- $alive ====="
    ask "$mon" "info status"        > "$OUT/run$i.status.txt"
    ask "$mon" "info registers -a" 2 > "$OUT/run$i.regs1.txt"
    sleep 2
    ask "$mon" "info registers -a" 2 > "$OUT/run$i.regs2.txt"
    ask "$mon" "info pci"          2 > "$OUT/run$i.pci.txt"
    ask "$mon" 'x/8i $eip'          > "$OUT/run$i.disas.txt"

    echo "    status: $(grep -oiE 'VM status: [a-z]+' "$OUT/run$i.status.txt" | head -1)"
    echo "    RIPs snapshot1: $(grep -oE 'RIP=[0-9a-f]+' "$OUT/run$i.regs1.txt" | tr '\n' ' ')"
    echo "    RIPs snapshot2: $(grep -oE 'RIP=[0-9a-f]+' "$OUT/run$i.regs2.txt" | tr '\n' ' ')"
    echo "    RAX/RDX cpu0 s1: $(grep -oE 'RAX=[0-9a-f]+|RDX=[0-9a-f]+' "$OUT/run$i.regs1.txt" | head -2 | tr '\n' ' ')"
    echo "    RAX/RDX cpu0 s2: $(grep -oE 'RAX=[0-9a-f]+|RDX=[0-9a-f]+' "$OUT/run$i.regs2.txt" | head -2 | tr '\n' ' ')"
    abar=$(grep -A6 -i "SATA controller" "$OUT/run$i.pci.txt" | grep -oiE "BAR5: 32 bit memory at 0x[0-9a-f]+" | grep -oE "0x[0-9a-f]+" | head -1)
    echo "    ABAR=${abar:-UNKNOWN}"
    echo "    --- disassembly at rip ---"; grep -E "^0x" "$OUT/run$i.disas.txt" | head -8 | sed 's/^/      /'
    # attribute the RIP: OVMF DEBUG prints every image's load address
    grep -aoE "Loading driver at 0x[0-9A-Fa-f]+ EntryPoint=0x[0-9A-Fa-f]+ [A-Za-z0-9_.]+" "$dbg" 2>/dev/null \
      | sort -u > "$OUT/run$i.loadmap.txt"
    echo "    load-map entries: $(wc -l < "$OUT/run$i.loadmap.txt")"
  fi
  kill -9 $qpid 2>/dev/null; wait $qpid 2>/dev/null
  killall -9 "$(basename "$QEMU")" 2>/dev/null; sleep 1
  rm -f "$OUT/run$i.vars.fd"
done
echo; echo "RESULT: $ok booted, $no noboot of $N"
