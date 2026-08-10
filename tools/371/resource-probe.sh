#!/bin/bash
# #371 round 2: is the bannerless boot caused by host resource exhaustion?
#
# The A/B established the failure is firmware-side, not hype's. What it did NOT establish is WHY,
# and one observation points at the environment: hype's four failures fell in its last six runs.
# That fits something degrading across a long series of boots.
#
# Three things this collects that the A/B did not:
#
#   1. HOST STATE before and after every boot -- MemFree/MemAvailable/Cached, load, and PSI
#      (pressure-stall) counters for cpu/memory/io. PSI is the direct measure of "something waited
#      for a resource", which loadavg only approximates.
#   2. QEMU's STDERR, per run. The previous script discarded it. If QEMU cannot get memory it says
#      so there, and I threw that away -- an allocation failure would have looked exactly like the
#      silent failure under investigation.
#   3. OVMF's own DEBUG output via -debugcon. On a failing boot the serial log contains nothing
#      from BDS at all, so this is the only channel that can say what the firmware decided.
#
# One arm only (the minimal app), because the payload has been eliminated as a variable and this
# doubles the samples per unit time. Same -m 8192 footprint, so the memory pressure is unchanged.
cd /mnt/data/dev/hype
N="${1:-30}"; SECS="${2:-22}"
ESP=disk-images/esp-371-hello.img
OUT=/tmp/claude-1000/-mnt-data-dev-hype/856105a5-c1a1-4cdd-b15f-f7cce249ea7b/scratchpad/res371
mkdir -p "$OUT"
CSV="$OUT/stats.csv"
echo "run,outcome,secs_to_exit,memfree_kb,memavail_kb,cached_kb,load1,psi_mem_some,psi_cpu_some,psi_io_some,qemu_stderr_bytes,dbg_bytes" > "$CSV"

psi() { LC_ALL=C awk -v k="$2" '$1==k {sub("total=","",$5); print $5}' "/proc/pressure/$1" 2>/dev/null | head -1; }
meminfo() { LC_ALL=C awk -v k="$1:" '$1==k {print $2}' /proc/meminfo; }

ok=0; no=0
for i in $(seq 1 "$N"); do
  LOG="$OUT/r$i.log"; ERR="$OUT/r$i.stderr"; DBG="$OUT/r$i.debugcon"
  cp -f /usr/share/edk2/ovmf/OVMF_VARS.fd "$OUT/r$i.vars.fd"
  rm -f "$LOG" "$ERR" "$DBG"

  mf=$(meminfo MemFree); ma=$(meminfo MemAvailable); ca=$(meminfo Cached)
  l1=$(cut -d' ' -f1 /proc/loadavg)
  pm=$(psi memory some); pc=$(psi cpu some); pio=$(psi io some)

  t0=$(date +%s)
  qemu-system-x86_64 -machine q35 -m 8192 -nodefaults -accel kvm -cpu host -smp 4 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
    -drive if=pflash,format=raw,file="$OUT/r$i.vars.fd" \
    -drive format=raw,file="$ESP" \
    -debugcon "file:$DBG" -global isa-debugcon.iobase=0x402 \
    -serial "file:$LOG" -display none -vga std 2>"$ERR" &
  qpid=$!
  for t in $(seq "$SECS"); do kill -0 $qpid 2>/dev/null || break; sleep 1; done
  t1=$(date +%s)
  kill -9 $qpid 2>/dev/null; wait $qpid 2>/dev/null
  killall -9 qemu-system-x86_64 2>/dev/null; sleep 1

  hit=$(LC_ALL=C grep -ac "hello: build" "$LOG" 2>/dev/null || echo 0)
  eb=$(wc -c < "$ERR"); db=$(wc -c < "$DBG" 2>/dev/null || echo 0)
  if [ "$hit" -gt 0 ]; then outcome=BOOTED; ok=$((ok+1)); else outcome=NOBOOT; no=$((no+1)); fi
  echo "$i,$outcome,$((t1-t0)),$mf,$ma,$ca,$l1,$pm,$pc,$pio,$eb,$db" >> "$CSV"
  echo "run $i: $outcome exit_after=$((t1-t0))s memfree=$((mf/1024))MB avail=$((ma/1024))MB load=$l1 stderr=${eb}B debugcon=${db}B"
  rm -f "$OUT/r$i.vars.fd"
done
echo
echo "RESULT: $ok booted, $no noboot, in $N runs"
echo "csv: $CSV"
