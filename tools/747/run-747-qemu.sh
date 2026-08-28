#!/bin/bash
# #747: a departing MASS-STORAGE device must fail its I/O, not corrupt it.
#
# A keyboard leaving costs a keystroke. hype BOOTS from USB mass storage and writes its own
# log there, so an unplug mid-write is a torn write, not an enumeration event. #596 is the
# precedent for how that goes unnoticed; this drive's own USB-SATA bridge dropped its link
# under sustained write during staging on 2026-08-27, with no operator involved.
#
# MEDIA=usb is not optional here: it is what makes the ESP a usb-storage device, so hype
# boots from USB, streams the guest ISO from USB and writes its log to USB -- one device,
# the 5950X's own shape. With the default IDE ESP the stick is just a spare volume hype
# declines ("not the volume hype booted from"), and deleting it would prove nothing.
#
# The unplug is `device_del` on the monitor: a real USB disconnect, raising the same Port
# Status Change Event hardware does.
#
# PASS = hype NOTICES, says so on the serial console (not through the log sink that just
# died), and STAYS UP. Failing to notice, or wedging, or continuing to write into FAT state
# that describes a device which is gone, are all failures.
set -e
. "$(git rev-parse --show-toplevel)/tools/qemu-env.sh"
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-disk-images/747}"
B=build
SECS="${1:-600}"
SMP="${SMP:--smp cpus=8,cores=4,threads=2,sockets=1}"
# MEDIA=usb puts EVERYTHING on one emulated USB device -- hype boots from it,
# streams the ISO from it and writes its log to it, which is the 5950X's own
# shape (one drive, hype on partition 1, the ISO on partition 2). Every other
# rig in this tree serves the ESP over IDE and uses USB only for the log, so
# the whole boot+media+log path shares no lock and no controller with itself.
# That is the largest remaining difference between the rigs that PASS and the
# hardware that does not, so it is a switch rather than a second file.
MEDIA="${MEDIA:-usb}"   # #747 needs hype booted FROM the device it will lose
CPU="${CPUFLAGS:-host,topoext=on}"
ISO="${ISO:-disk-images/alpine-virt-console.iso}"
CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE.fd}
VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS.fd}
mkdir -p "$S"
killall -9 "$(basename "$QEMU")" 2>/dev/null || true; sleep 1
pidof qemu-system-x86_64 >/dev/null && { echo "FAIL: a qemu is still running"; exit 1; }

rm -f $S/usb.img $S/esp.img $S/serial.txt
dd if=/dev/zero of=$S/usb.img bs=1M count=64 status=none
mkfs.vfat -F 32 -n HYPEUSB $S/usb.img >/dev/null

cat > $S/hype.cfg <<'CFG'
[hype]
config_version = 1

[vm.run1a]
label = run735
vcpus = 2
mem_mb = 1024
boot = installer
install_media = \iso\test.iso
firmware = uefi
os_hint = linux
target_disk = file:\hype\disks\run747.img
CFG

dd if=/dev/zero of=$S/esp.img bs=1M count=512 status=none
sfdisk -q $S/esp.img <<'PT'
label: gpt
start=2048, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B
PT
E="$S/esp.img@@1M"
mkfs.vfat -F 32 -n HYPEESP --offset 2048 $S/esp.img >/dev/null
mmd -i "$E" ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/input ::/hype ::/hype/disks
mcopy -i "$E" $B/hype.efi ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$E" fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
mcopy -i "$E" "$ISO" ::/iso/test.iso
mcopy -i "$E" $S/hype.cfg ::/hype.cfg
# #747 needs the guest WRITING at the moment of the unplug, not sitting at a login prompt.
mcopy -i "$E" tools/747/input-write/vm0.txt ::/input/vm0.txt
# The guest's disk, on the ESP -- which under MEDIA=usb is the device that gets pulled.
dd if=/dev/zero of=$S/run747.img bs=1M count=32 status=none
mcopy -i "$E" $S/run747.img ::/hype/disks/run747.img

if [ "$MEDIA" = usb ]; then
  ESPDEV=(-drive format=raw,file=$S/esp.img,if=none,id=esp
          -device usb-storage,id=espdev,bus=xhci.0,drive=esp,bootindex=0,serial=HYPE747ESP)
else
  ESPDEV=(-drive format=raw,file=$S/esp.img,if=none,id=esp
          -device ide-hd,drive=esp,bus=ide.0,bootindex=0)
fi
echo "media path: $MEDIA (esp over ${MEDIA})"
cp "$VARS" $S/VARS.fd
(
  # Wait until hype is genuinely using the medium -- a log sink open and a guest reading
  # its ISO -- so the unplug lands on live I/O rather than on an idle device.
  for _ in $(seq 1 "$SECS"); do
    sleep 1
    grep -aq "WRITING-NOW" $S/serial.txt 2>/dev/null && break
  done
  # Long enough that dd is past its first requests and there is genuinely I/O in flight,
  # short enough that the 32 MiB disk has not been written end to end.
  sleep 8
  printf 'device_del espdev\n'
  sleep 40
  # #747 clause 4: re-plug, the SAME backing file. If anything resumed it would resume onto
  # the half-written chain, which is exactly what this clause exists to catch.
  printf 'device_add usb-storage,bus=xhci.0,drive=esp,id=espdev,serial=HYPE747ESP\n'
  sleep 30
  printf 'quit\n'
) | timeout "$SECS" "$QEMU" \
  -machine q35 -m 4096 -nodefaults \
  -accel kvm -accel tcg -cpu "$CPU" $SMP \
  -drive if=pflash,format=raw,readonly=on,file="$CODE" \
  -drive if=pflash,format=raw,file=$S/VARS.fd \
  -device qemu-xhci,id=xhci \
  "${ESPDEV[@]}" \
  -drive format=raw,file=$S/usb.img,if=none,id=stick \
  -device usb-storage,bus=xhci.0,drive=stick,serial=HYPE735LOG \
  -serial "file:$S/serial.txt" -monitor stdio -display none -vga none >$S/mon.log 2>&1 || true

echo "=== did hype notice the medium leaving? ==="
grep -a "DEPARTED\|port .* changed -- now empty" $S/serial.txt | head -6

echo "=== verdict ==="
rc=0
if ! grep -aq 'hype: build' $S/serial.txt; then echo "NOBOOT: hype never ran (#371)"; exit 2; fi
if ! grep -aq 'usb-log:\|host-usb: INVENTORY' $S/serial.txt; then
  echo "FAIL: hype never got as far as using the USB medium -- nothing to unplug"; exit 1
fi

# 1. it must NOTICE.
if grep -aq 'host-usb: the LOG/BOOT medium .* DEPARTED' $S/serial.txt; then
  echo "PASS: the boot/log medium's departure was noticed [#747]"
elif grep -aq 'host-usb: media device .* DEPARTED' $S/serial.txt; then
  echo "PASS: a media device's departure was noticed [#747]"
elif grep -aq 'port .* changed -- now empty' $S/serial.txt; then
  echo "FAIL: the PORT change was seen but the storage backend was not marked gone [#747]"
  rc=1
else
  echo "FAIL: the departure was not noticed at all [#744/#747]"; rc=1
fi

# 2. it must say so on the SERIAL console. The log sink is the thing that just died, so a
#    message routed through it would be the one message guaranteed to be lost.
grep -aq 'host-usb: the LOG/BOOT medium .* the log sink is abandoned' $S/serial.txt &&
  echo "PASS: reported on serial, not through the sink that died [#747]"

# 3. hype must STAY UP. A departing medium must not take the hypervisor with it -- that is
#    the difference between losing a log and losing the machine.
N=$(grep -a -c "fw-1 DIAG:" $S/serial.txt)
LAST=$(grep -a -n "DEPARTED" $S/serial.txt | tail -1 | cut -d: -f1)
AFTER=$(tail -n +"${LAST:-1}" $S/serial.txt | grep -a -c "fw-1 DIAG:")
echo "DIAG lines: $N total, $AFTER after the departure"
if [ "${AFTER:-0}" -lt 2 ]; then
  echo "FAIL: hype produced almost nothing after the unplug -- it wedged with the device [#747]"
  rc=1
else
  echo "PASS: hype kept running and kept reporting after the unplug [#747]"
fi

# 4. and it must not keep hammering a device that is gone.
STUCK=$(grep -a -c "usb-log: BEHIND" $S/serial.txt)
echo "usb-log BEHIND lines: $STUCK"

# 5. the I/O must be REFUSED, not merely un-attempted. This is the clause the rig could not
#    test before it drove a write: an idle device that departs logs identically to a busy
#    one, so detection was all that was ever being proved.
# 5a. FIRST: was the guest genuinely writing? Everything below is meaningless otherwise,
#     and an earlier version of this rig reported a healthy refusal count from hype's own
#     log writes while the guest's dd had been failing silently into /dev/null the whole
#     run. Prove the write before trusting the refusal.
echo "=== was the guest actually writing when it was pulled? ==="
grep -a "DISK-IS-\|WROTE-" $S/serial.txt | tail -2
WROTE=$(grep -a "WROTE-" $S/serial.txt | grep -oE 'WROTE-[0-9]+' | cut -d- -f2 | sort -n | tail -1)
if [ -z "${WROTE:-}" ]; then
  echo "FAIL: the guest never reported a write count -- the write loop did not start [#747]"
  rc=1
elif [ "$WROTE" -gt 0 ]; then
  echo "PASS: the guest had written $WROTE sectors before the pull [#747]"
else
  echo "FAIL: the guest wrote 0 sectors -- the pull landed on an IDLE device, so this run"
  echo "      proves detection only, which is what the rig already did [#747]"
  rc=1
fi

echo "=== was live I/O actually refused? ==="
grep -a "DIAG: GONE" $S/serial.txt | tail -3
REFUSED=$(grep -a "DIAG: GONE" $S/serial.txt | grep -oE 'refused=[0-9]+' | cut -d= -f2 | sort -n | tail -1)
if [ -z "$REFUSED" ]; then
  echo "FAIL: no GONE diagnostic at all -- nothing recorded the departed state [#747]"; rc=1
elif [ "$REFUSED" -gt 0 ]; then
  echo "PASS: $REFUSED requests refused with a named error, not timed out [#747]"
else
  echo "FAIL: the device is marked departed but refused=0 -- the guest was not writing, so"
  echo "      the refusal path was never exercised and this run proves only detection [#747]"
  rc=1
fi

# 6. the guest must be TOLD, not left hanging. The heartbeat is what makes this decidable:
#    it does not touch the disk, so if it keeps ticking the kernel is alive, and if it stops
#    the guest is blocked on a request that will never complete -- the failure the ticket
#    names. Counting console lines cannot separate those, and an earlier version of this rig
#    recorded "no guest-side I/O error" without being able to say which had happened.
echo "=== was the guest told, or left hanging? ==="
DEPLINE=$(grep -a -n "DEPARTED" $S/serial.txt | head -1 | cut -d: -f1)
HB_AFTER=$(tail -n +"${DEPLINE:-1}" $S/serial.txt | grep -a -c "GHB-")
echo "guest heartbeats after the departure: $HB_AFTER"
if [ "${HB_AFTER:-0}" -lt 2 ]; then
  echo "FAIL: the guest stopped ticking at the unplug -- blocked on I/O that will never"
  echo "      complete, which is exactly what clause 6 forbids [#747]"
  rc=1
else
  echo "PASS: the guest kept running after its disk vanished [#747]"
fi
if grep -aq "I/O error\|end_request\|Buffer I/O error\|blk_update_request\|dd: " $S/serial.txt; then
  echo "PASS: and it was told -- a guest-visible error, not a silent stall [#747]"
else
  echo "FAIL: the guest kept running but was never told its write failed. A write that"
  echo "      silently does nothing is the torn-write case this ticket exists for [#747]"
  rc=1
fi

# 7. a re-plug must NOT silently resume. The sticky flag is the whole design: a half-written
#    chain is not made good by the device coming back.
echo "=== did anything resume on the re-plug? ==="
RELINE=$(grep -a -n "device_add\|ARRIVED" $S/serial.txt | tail -1 | cut -d: -f1)
if grep -aq "usb-log:.*opened" <(tail -n +"${RELINE:-1}" $S/serial.txt) 2>/dev/null; then
  echo "FAIL: the log sink re-opened by itself after the re-plug -- departed must be sticky"
  echo "      until an explicit attach [#747]"
  rc=1
else
  echo "PASS: nothing resumed on its own; re-attach stays an operator action [#747]"
fi
LASTGONE=$(grep -a "DIAG: GONE" $S/serial.txt | tail -1)
if [ -n "$LASTGONE" ]; then
  echo "PASS: still reporting GONE after the re-plug: $LASTGONE"
fi

# 8. and the medium must not have been left corrupt. #596 is the precedent: a broken cluster
#    chain that a resume would extend. fsck reads the image the guest and hype were both
#    writing when it was pulled.
echo "=== filesystem integrity of the pulled medium ==="
if command -v fsck.vfat >/dev/null 2>&1; then
  # fsck.vfat has no offset option -- @@1M is mtools syntax, and passing it just gets
  # "open: No such file or directory". Cut the partition out instead; 2048 sectors in,
  # matching the sfdisk layout above.
  dd if=$S/esp.img of=$S/esp-part.img bs=512 skip=2048 status=none
  # -n = read-only check. Non-zero means dirty or inconsistent.
  if fsck.vfat -n "$S/esp-part.img" >$S/fsck.txt 2>&1; then
    echo "PASS: the ESP FAT is consistent after the pull [#747 #596]"
  else
    echo "FAIL: the ESP FAT is inconsistent after the pull -- a torn write survived [#747]"
    sed -n '1,12p' $S/fsck.txt
    rc=1
  fi
else
  echo "SKIP: fsck.vfat not installed -- integrity unverified"
fi
exit "$rc"
