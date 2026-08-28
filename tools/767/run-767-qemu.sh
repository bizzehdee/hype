#!/bin/bash
#
# #767: a QEMU rig whose USB topology mirrors the 5950X.
#
# Eight hardware boots found USB defects that had "passed in QEMU". docs/qemu-vs-hardware.md
# audits them: two genuinely needed hardware, one was blocked by a false claim written into
# rig 746, and the rest were reachable here with a topology closer to the real machine.
#
# The rigs modelled ONE controller, ONE hub, four devices and three interrupt-IN endpoints.
# The desk has two controllers, five hub devices, twelve devices and eight endpoints. Several
# defects are invisible below a threshold the small rigs never crossed:
#
#   #759  the idle-poll cost is PER interrupt-IN endpoint      3 endpoints vs 8
#   #765  the hub pool held 6 hub devices                      1 hub vs 5
#   #757  stale port bits need a busy enumeration to bank      4 clean devices vs 12
#   #769  a controller with no polled endpoint is never drained  needs a SECOND controller
#   #768  a slot id is per-controller                          needs the same slot id on two
#
# What the 5950X actually is, from the boot-13 inventory and lsusb -v:
#
#   ctrl1  port4   152d:1561  JMicron SATA bridge -- hype's boot and log medium
#          port6   05e3:0608  Genesys 2.0 hub (motherboard-internal)
#          port6/4 1e71:2007  NZXT                        (vendor class)
#          port12  1462:7c91  MSI Mystic Light            (vendor class)
#   ctrl2  port1   1bcf:2284  UGREEN camera 2K            (misc class)
#          port3   0bda:5411  Realtek 2.0 hub
#          port4   05e3:0610  Genesys 2.1 hub  <- the keyboard's hub
#          port4/1 eba4:6579  UGREEN Camera 4K            (misc class)
#          port4/2 3434:0da4  Keychron K10, 3 interfaces  <- the keyboard
#          port4/4 046d:c547  Logitech receiver, boot kbd AND boot mouse
#          port7   0bda:0411  Realtek hub (3.0 half); its port 1 refuses Address Device
#          port8   05e3:0626  Genesys hub (3.0 half)
#
# THE DELIBERATE ARRANGEMENT, and the reason this rig exists:
#
#   * the boot/log medium is on xhci0, and EVERY human-input device is on xhci1. That is the
#     5950X's shape and it is what exposed #769: xhci0 has no polled interrupt-IN endpoint,
#     so nothing drains its event ring except the guest's own ISO reads, and once the kernel
#     has loaded its root ports go blind.
#   * a hub behind a hub on xhci1. A keyboard with a built-in hub plugged into a physical hub
#     is ordinary and presents exactly this way.
#   * four hub devices, so the per-endpoint costs are near the real machine's.
#
# THE COMPOSITE. Stock QEMU has no HID with two interfaces, so the path where hype claims one
# device TWICE on one slot -- once per interface -- had nothing to run against, and #755 and
# #771 both reached hardware unreproduced. `usb-kbd-mouse` is a local addition to
# hw/usb/dev-hid.c: idVendor/idProduct 046d:c547, interface 0 a boot mouse on endpoint 1,
# interface 1 a boot keyboard on endpoint 2 -- the receiver's exact shape. It is carried as
# tools/767/qemu-composite-hid.patch.
#
# THE UNADDRESSABLE DEVICE. `usb-badaddr` stalls every control transfer, so SET_ADDRESS never
# completes and the host sees what the 5950X's nested SuperSpeed hub does: Address Device
# cc=4, every time. That drove #763 (retries without bound) and #770 (one port reported 4,610
# times), both of which reached hardware unreproduced because nothing in QEMU refuses to be
# addressed. Also carried in tools/767/qemu-composite-hid.patch.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S=rig/i767
SECS="${1:-900}"
mkdir -p $S
rm -f $S/serial.log $S/esp.img $S/usb.img

killall -9 "$(basename "$QEMU")" 2>/dev/null || true
sleep 1
pidof qemu-system-x86_64 >/dev/null && { echo "FAIL: a qemu is still running"; exit 1; }

dd if=/dev/zero of=$S/usb.img bs=1M count=64 status=none
mkfs.vfat -F 32 -n HYPEUSB $S/usb.img >/dev/null
dd if=/dev/zero of=$S/esp.img bs=1M count=512 status=none
mkfs.vfat -F 32 -n HYPEESP $S/esp.img >/dev/null
mmd -i $S/esp.img ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/hype ::/hype/disks
mcopy -i $S/esp.img build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i $S/esp.img fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i $S/esp.img disk-images/alpine-virt-console.iso ::/iso/test.iso
mcopy -i $S/esp.img tools/734/hype.cfg ::/hype.cfg
cp /usr/share/edk2/ovmf/OVMF_VARS.fd $S/VARS.fd

(
  # Wait for the GUEST to be up, not just for hype's first HID dump. The dump appears
  # within a minute; the guest takes several. Hot-plugging before the guest has booted
  # leaves the run out of time before anything is typed, which reads as "the keyboard
  # produced no scancodes" when nothing had asked it to.
  # Wait for the guest, but BOUNDED. An earlier version waited up to $SECS -- the same
  # budget the whole QEMU run has -- so on a boot that stalled it consumed the entire run
  # and not one key was ever sent. That reads as "the keyboard produced no scancodes" when
  # nothing had asked it to. The USB paths below are hype-side and do not need the guest;
  # the guest only matters because its ISO reads are what keep controller 0 busy.
  for _ in $(seq 1 240); do
    sleep 1
    grep -aq "localhost login:" $S/serial.log 2>/dev/null && break
  done
  sleep 5
  # Type and move for a while, so the endpoints are genuinely in use.
  for _ in $(seq 1 80); do
    printf 'sendkey a\n'; sleep 0.15
    printf 'mouse_move 5 5\n'; sleep 0.15
  done
  sleep 20

  # 1. the keyboard leaves and returns BEHIND THE HUB (#746 path)
  printf 'device_del kbd0\n';  sleep 12
  printf 'device_add usb-kbd,id=kbd0,bus=xhci1.0,port=1.2\n'; sleep 12
  for _ in $(seq 1 40); do printf 'sendkey b\n'; sleep 0.15; done

  # 2. a device arrives on the OTHER controller's root port -- the #769 case. xhci0 has no
  #    polled endpoint of its own, so this is only seen if the sweep pumps every controller.
  printf 'device_add usb-kbd,id=kbd1,bus=xhci0.0,port=3\n'; sleep 12
  for _ in $(seq 1 40); do printf 'sendkey d\n'; sleep 0.15; done
  printf 'device_del kbd1\n'; sleep 12
  printf 'device_add usb-kbd,id=kbd1,bus=xhci0.0,port=3\n'; sleep 12
  for _ in $(seq 1 40); do printf 'sendkey e\n'; sleep 0.15; done

  # 3. THE WHOLE HUB leaves, with the keyboard and mouse still in it, and comes back.
  printf 'device_del hubA\n'; sleep 20
  printf 'device_add usb-hub,id=hubA,bus=xhci1.0,port=1,ports=4\n'; sleep 8
  printf 'device_add usb-kbd,id=kbd0,bus=xhci1.0,port=1.2\n'; sleep 8
  printf 'device_add usb-mouse,id=mou0,bus=xhci1.0,port=1.4\n'; sleep 12
  for _ in $(seq 1 60); do printf 'sendkey f\n'; sleep 0.15; done
  sleep 30
  printf 'quit\n'
) | timeout "$SECS" "$QEMU" \
  -machine q35 -m 4096 -nodefaults \
  -accel kvm -cpu host,topoext=on -smp cpus=4,sockets=1,cores=2,threads=2 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=$S/VARS.fd \
  -drive format=raw,file=$S/esp.img,if=none,id=esp \
  -device ide-hd,drive=esp,bus=ide.0,bootindex=0 \
  -device qemu-xhci,id=xhci0 \
  -device qemu-xhci,id=xhci1 \
  -drive format=raw,file=$S/usb.img,if=none,id=stick \
  -device usb-storage,bus=xhci0.0,port=1,drive=stick,serial=HYPE767USB \
  -device usb-hub,id=hubI,bus=xhci0.0,port=2,ports=4 \
  -device usb-hub,id=hubA,bus=xhci1.0,port=1,ports=4 \
  -device usb-kbd,id=kbd0,bus=xhci1.0,port=1.2 \
  -device usb-kbd-mouse,id=combo,bus=xhci1.0,port=1.3 \
  -device usb-mouse,id=mou0,bus=xhci1.0,port=1.4 \
  -device usb-hub,id=hubN,bus=xhci1.0,port=1.1,ports=4 \
  -device usb-tablet,id=tab0,bus=xhci1.0,port=1.1.1 \
  -device usb-hub,id=hubB,bus=xhci1.0,port=2,ports=4 \
  -device usb-badaddr,id=bad0,bus=xhci1.0,port=2.1 \
  -serial "file:$S/serial.log" -monitor stdio -display none -vga std >$S/mon.log 2>$S/qemu.err || true

echo "=== topology hype found ==="
grep -a "INVENTORY --" $S/serial.log | tail -1
grep -a "hub-devices=" $S/serial.log | tail -1
echo "=== claims ==="
grep -a "host-hid: USB .* CLAIMED" $S/serial.log
echo "=== hot-plug ladder ==="
grep -a "PORT EVENT\|on this controller changed\|hub slot .* changed\|ARRIVED\|DEPARTED" $S/serial.log | tail -20

fail=0
say() { echo "$@"; fail=1; }
echo "=== verdict ==="
grep -aq "hype: build" $S/serial.log || { echo "NOBOOT: hype never ran (#371)"; exit 2; }

# The topology itself, or the rig is not doing its job.
nctl=$(grep -a "INVENTORY --" $S/serial.log | tail -1 | sed -n 's/.*across \([0-9]*\) controller.*/\1/p')
[ "${nctl:-0}" -ge 2 ] 2>/dev/null || say "FAIL: only ${nctl:-0} controller(s) -- the #769 case needs two"
nhub=$(grep -a "hub-devices=" $S/serial.log | tail -1 | sed -n 's/.*hub-devices=\([0-9]*\).*/\1/p')
[ "${nhub:-0}" -ge 4 ] 2>/dev/null || say "FAIL: only ${nhub:-0} hub device(s) -- expected 4"

# NOTE on port numbers: QEMU's `port=` property is a bus path, not hype's root-port index
# (xHCI numbers USB2 ports then USB3 ports), so these assertions count EVENTS and name
# devices rather than hardcoding a port. An earlier version asserted port 1 and port 3 and
# failed on a run where everything had worked.

# #769: arrivals on the controller with no polled endpoint of its own. kbd1 is added, removed
# and re-added there, so a working sweep sees at least two arrivals beyond the behind-hub one.
narr=$(grep -a -c "host-usb: port .* ARRIVED" $S/serial.log || true)
echo "root-port arrivals: $narr"
[ "${narr:-0}" -ge 2 ] 2>/dev/null ||
  say "FAIL: only ${narr:-0} root-port arrival(s) -- a controller with no polled endpoint is
      not being drained, so its later plugs are invisible [#769]"

# #746: behind-hub departure and return.
grep -aq "behind hub slot .* DEPARTED" $S/serial.log ||
  say "FAIL: the behind-hub departure was never acted on [#746]"

# The hub ITSELF leaving, which boots 14 and 15 could not manage. The test is not just that
# the port went empty -- it is that EVERYTHING behind the hub was released with it.
if grep -aq "on this controller changed -- now empty" $S/serial.log; then
  rel=$(grep -a -A6 "on this controller changed -- now empty" $S/serial.log |
        grep -a -c "DEPARTED -- releasing" || true)
  echo "devices released when a root port emptied: $rel"
  [ "${rel:-0}" -ge 2 ] 2>/dev/null ||
    say "FAIL: a hub left but only ${rel:-0} device(s) behind it were released [#770]"
else
  say "FAIL: unplugging the HUB ITSELF was never noticed [#770]"
fi

# #770: reporting must stay bounded.
hr=$(grep -a "hub-devices=" $S/serial.log | tail -1 | sed -n 's/.*reports=\([0-9]*\).*/\1/p')
echo "hub reports: ${hr:-0}"
[ "${hr:-0}" -lt 500 ] 2>/dev/null ||
  say "FAIL: the hubs reported ${hr} times -- change bits are not clearing [#762 #770]"

# #771: no endpoint claimed twice AT THE SAME TIME. Two claims separated by a departure are
# a re-plug, not a duplicate, so this looks only at the LAST dump.
nlast=$(grep -a -o "fw-1 DIAG: HID\[[0-9]*/[0-9]*\]" $S/serial.log | tail -1 |
        sed -n 's|.*/\([0-9]*\)\]|\1|p')
dup=$(grep -a "fw-1 DIAG: HID\[" $S/serial.log | grep -v modseen | tail -"${nlast:-1}" |
      grep -oE 'slot[0-9]+ ep=0x[0-9a-f]+' | sort | uniq -d || true)
[ -z "$dup" ] || say "FAIL: the same endpoint is claimed twice in one dump ($dup) [#771]"

# #764/#766: the ring must not drift. `grep -c` exits non-zero on zero matches and this
# script runs under `set -e`, which silently truncated an earlier version of this verdict.
# #764: ANY divergence fails. An earlier version only counted the late "NOT re-armed" line
# and passed a run in which a hot-plugged keyboard had enumerated, been claimed, and never
# reported -- because a different keyboard was carrying the scancodes. One deaf endpoint is
# the whole bug; it does not stop being one because another device is healthy.
ndv=$(grep -a -c "#764 DIVERGED" $S/serial.log || true)
echo "endpoints that diverged: $ndv"
[ "${ndv:-0}" -eq 0 ] 2>/dev/null || {
  grep -a "#764 DIVERGED\|#764   claim" $S/serial.log | head -12
  say "FAIL: $ndv endpoint(s) lost track of the controller's dequeue pointer [#764]"
}
nsp=$(grep -a -c "#764 SLOW POLL" $S/serial.log || true)
echo "slow polls (>20ms): $nsp"
[ "${nsp:-0}" -eq 0 ] 2>/dev/null ||
  say "FAIL: $nsp interrupt-IN poll(s) blocked for over 20 ms -- that is a visible freeze [#764]"

nd=$(grep -a -c "NOT re-armed" $S/serial.log || true)
echo "late drift reports: $nd"
[ "${nd:-0}" -eq 0 ] 2>/dev/null ||
  say "FAIL: $nd interrupt-IN endpoint(s) drifted from the controller's dequeue pointer [#764 #766]"

# A keyboard must still be delivering at the end. QEMU routes sendkey to ONE keyboard
# handler, so with both a plain usb-kbd and the composite present only one of them will
# ever carry the keys -- which is why this checks the MERGED counter rather than any
# single HID entry.
last=$(grep -a "fw-1 DIAG: host-kbd" $S/serial.log | tail -1 | sed -n 's/.*scancodes=\([0-9]*\).*/\1/p')
echo "merged scancodes at the end: ${last:-0}"
[ "${last:-0}" -gt 0 ] 2>/dev/null ||
  say "FAIL: no keyboard delivered a single scancode"

echo
# The composite must be claimed TWICE -- once per interface -- on ONE slot, which is the
# whole reason it is here.
ncombo=$(grep -a "host-hid: USB .* CLAIMED -- 046d:c547" $S/serial.log | wc -l)
echo "composite claims (want 2: one keyboard interface, one mouse interface): $ncombo"
[ "${ncombo:-0}" -ge 2 ] 2>/dev/null ||
  say "FAIL: the composite was claimed ${ncombo:-0} time(s) -- the claim-twice path is not
      being exercised, which is what let #755 and #771 reach hardware [#767]"
# and its two endpoints must differ, or one claim overwrote the other
ceps=$(grep -a "host-hid: USB .* CLAIMED -- 046d:c547" $S/serial.log | grep -oE 'ep=0x[0-9a-f]+' | sort -u | wc -l)
[ "${ceps:-0}" -ge 2 ] 2>/dev/null ||
  say "FAIL: both composite claims landed on the same endpoint -- #755's Context Entries
      clobber, or #771's duplicate [#755 #771]"

echo
# #763/#770: the unaddressable device must be given up on, and the hub must then stop being
# asked about its port. Unbounded retries are what starved the input tick on hardware.
# NOTE: usb-badaddr stalls control transfers, and QEMU's xHCI model performs SET_ADDRESS
# itself, so the failure surfaces at GET_DESCRIPTOR rather than Address Device. Either way it
# is a device that cannot be enumerated, which is the property #763 and #770 care about.
nfail=$(grep -a -c "GET_DESCRIPTOR FAILED\|Address Device slot .* completion code" $S/serial.log || true)
echo "failed-enumeration attempts: $nfail"
grep -aq "GET_DESCRIPTOR FAILED\|Address Device .* FAILED" $S/serial.log ||
  say "FAIL: the unenumerable device was enumerated fine -- usb-badaddr is not doing its job"
[ "${nfail:-0}" -lt 40 ] 2>/dev/null ||
  say "FAIL: $nfail failed enumeration attempts -- a device that cannot be enumerated is
      being retried without bound, which is what starved the input tick on hardware [#763]"
# and the hub must not then report that port without end (#770)
nstorm=$(grep -a -c "hub slot .* changed" $S/serial.log || true)
echo "hub port-change reports: $nstorm"
[ "${nstorm:-0}" -lt 200 ] 2>/dev/null ||
  say "FAIL: $nstorm hub port-change reports -- a port hype gave up on is still being
      offered [#770]"

[ $fail -eq 0 ] && echo "ALL PASS: two controllers, four hub devices, nested hubs, and every hot-plug path [#767]"
exit $fail
