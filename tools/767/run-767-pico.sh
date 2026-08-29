#!/bin/sh
#
# #778: the same physical keyboard, on QEMU's xHCI instead of the 5950X's.
#
# This is the correlation rig the USB investigation has lacked. Every previous comparison ran
# EMULATED devices in QEMU against REAL devices on hardware, so a difference in outcome could
# always be blamed on the device models -- and twice was, wrongly. Passing the Pico through
# means ONE physical device, ONE firmware, ONE fixed script, on two controllers.
#
# If the script survives here and dies on the desk, the difference is the CONTROLLER or its
# transaction translator, because nothing else changed. That is the axis the deafness sits on
# and the one no rig could isolate.
#
# It costs the operator nothing: the Pico is not their keyboard, so unlike passing through
# the Keychron there is no window in which the host has no input.
#
# The topology is #767's, minus the emulated keyboard on the hub port the Pico takes, so the
# two rigs stay comparable.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S=rig/i778
SECS="${1:-3600}"

VID=cafe
PID=4b44

# The device node must be readable, or qemu starts WITHOUT the keyboard and the run looks
# like a total input failure -- indistinguishable from the bug being chased.
NODE=$(lsusb | awk -v v="$VID:$PID" 'index($6,v)==1 {gsub(":","",$4); printf "/dev/bus/usb/%s/%s", $2, $4}')
[ -n "$NODE" ] || { echo "FAIL: no $VID:$PID on the bus -- is the Pico plugged in and flashed?"; exit 1; }
# WRITABLE, not merely readable. qemu must claim the device and detach the kernel driver,
# and both need write access. An earlier version of this check tested -r, passed on a
# read-only node, and qemu started WITHOUT the keyboard -- so the Pico went on typing into
# the operator's desktop while the run looked like a total input failure. That is the exact
# silent failure this check exists to prevent, so it must test what qemu actually needs.
[ -w "$NODE" ] || {
    echo "FAIL: $NODE is not WRITABLE, so qemu cannot claim the Pico."
    echo "  sudo tee /etc/udev/rules.d/99-hype-pico.rules <<'RULE'"
    echo "  SUBSYSTEM==\"usb\", ATTR{idVendor}==\"$VID\", ATTR{idProduct}==\"$PID\", MODE=\"0666\""
    echo "RULE"
    echo "  sudo udevadm control --reload && sudo udevadm trigger"
    exit 1
}
echo "pico: $NODE (writable)"

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

# No scripted sendkey here: the KEYBOARD drives the test. That is the whole point -- the same
# keystrokes, from the same firmware, as the hardware run.
timeout "$SECS" "$QEMU" \
  -machine q35 -m 4096 -nodefaults \
  -accel kvm -cpu host,topoext=on -smp cpus=4,sockets=1,cores=2,threads=2 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=$S/VARS.fd \
  -drive format=raw,file=$S/esp.img,if=none,id=esp \
  -device ide-hd,drive=esp,bus=ide.0,bootindex=0 \
  -device qemu-xhci,id=xhci0 \
  -device qemu-xhci,id=xhci1 \
  -drive format=raw,file=$S/usb.img,if=none,id=stick \
  -device usb-storage,bus=xhci0.0,port=1,drive=stick,serial=HYPE778USB \
  -device usb-hub,id=hubI,bus=xhci0.0,port=2,ports=4 \
  -device usb-hub,id=hubA,bus=xhci1.0,port=1,ports=4 \
  -device usb-host,id=pico,bus=xhci1.0,port=1.2,vendorid=0x$VID,productid=0x$PID \
  -device usb-kbd-mouse,id=combo,bus=xhci1.0,port=1.3 \
  -device usb-mouse,id=mou0,bus=xhci1.0,port=1.4 \
  -device usb-hub,id=hubN,bus=xhci1.0,port=1.1,ports=4 \
  -device usb-tablet,id=tab0,bus=xhci1.0,port=1.1.1 \
  -device usb-hub,id=hubB,bus=xhci1.0,port=2,ports=4 \
  -device usb-badaddr,id=bad0,bus=xhci1.0,port=2.1 \
  -serial "file:$S/serial.log" -display none -vga std >$S/mon.log 2>$S/qemu.err || true

echo "=== claims ==="
grep -a "host-hid: USB .* CLAIMED" $S/serial.log | tail -4
echo "=== the pico's tags, as hype saw them ==="
echo "last scancode total: $(grep -ao 'scancodes=[0-9]*' $S/serial.log | tail -1)"
echo "=== HIDTICK: where reports stopped, if they did ==="
grep -a "HIDTICK" $S/serial.log | tail -3
echo "=== self hot-plug cycles seen ==="
echo "departures: $(grep -ac 'DEPARTED' $S/serial.log)  arrivals: $(grep -ac 'ARRIVED' $S/serial.log)"
