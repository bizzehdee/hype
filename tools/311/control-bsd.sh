#!/bin/bash
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
# #343 CONTROL: FreeBSD under plain QEMU, NO hype in the picture at all.
#
# The same discipline tools/262/make-rig.sh already codifies -- "THE CONTROL RUNS FIRST" -- applied
# to a guest-side fault. Without it, "hype corrupts the guest" and "this FreeBSD is flaky on this
# machine" look identical, and #343 spent its whole life on the first reading without anyone testing
# the second.
#
# Cheap, because the fault is at 1s uptime: reaching `pci0:` is enough, no installer needed.
# A panic HERE would mean the fault is not hype's. A clean run does not by itself convict hype --
# the control boots the ISO directly while hype's guest reads a streamed virtual CD -- but it does
# rule out the ISO, the kernel and the host being independently unstable.
export LC_ALL=C
cd /mnt/data/dev/hype/disk-images
ok=0; bad=0
for i in $(seq 1 8); do
  killall -9 "$(basename "$QEMU")" 2>/dev/null; sleep 2
  cp -f /usr/share/edk2/ovmf/OVMF_VARS.fd cv.fd
  timeout 90 "$QEMU" -machine q35 -m 4096 -nodefaults -accel kvm -cpu host -smp 4 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
    -drive if=pflash,format=raw,file=cv.fd \
    -drive id=cd,if=none,format=raw,readonly=on,file=/home/darren/Downloads/FreeBSD-15.1-RELEASE-amd64-disc1.iso \
    -device ide-cd,drive=cd,bus=ide.0,bootindex=0 \
    -serial file:k$i.log -display none -vga none 2>/dev/null || true
  killall -9 "$(basename "$QEMU")" 2>/dev/null
  p=$(grep -acE 'panic:|Fatal trap' k$i.log); r=$(grep -ac 'pci0:' k$i.log)
  [ "$r" -gt 0 ] && ok=$((ok+1)) || bad=$((bad+1))
  echo "k$i: panic=$p reached_pci=$r bytes=$(wc -c < k$i.log)"
  if [ "$p" -gt 0 ]; then echo "=== CONTROL PANICKED ==="; grep -aE -A 6 'panic:' k$i.log | head -12; break; fi
done
echo "control done: $ok usable, $bad duds"
