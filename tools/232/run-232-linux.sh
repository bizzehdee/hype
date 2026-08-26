#!/bin/bash
# #232: prove the SEPARATE hype-additions ISO design end to end, through hype
# (not bare QEMU) -- boot 1 installs Alpine unattended using cdroms[0] (the
# tiny bridge ISO) + cdroms[1] (hype-additions.iso, all real content); boot 2
# is a fresh hype process with `boot = disk` and neither CD attached, proving
# the installed system is real and self-contained (same two-phase shape as
# #120's own boot=disk validation).
set -e
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
HERE=tools/232
B=${HYPE_232_BUILD:-disk-images/hype-232-build}
# Refuse to run twice at once, via an flock on this script itself.
#
# This matters because the line below kills every qemu at startup, so a second
# copy murders the first one's VM, which then does the same back on ITS next
# phase -- the two starve each other and neither ever boots. The symptom reads
# exactly like a firmware hang: a boot log stuck at 73 bytes (the outer OVMF's
# screen clear), qemu at 99% CPU, and an elapsed time that keeps resetting. It
# cost several misdiagnosed runs before six concurrent copies of this script
# turned up in `ps`.
#
# flock rather than a pgrep scan: `pgrep -f run-232-linux.sh` also matches the
# `nohup ./tools/232/run-232-linux.sh` wrapper, whose pid is not this shell's,
# so a pid-based guard refuses to start when nothing is actually running.
exec 9<"$0"
if ! flock -n 9; then
    echo "refusing to start: another run-232-linux.sh holds the lock" >&2
    echo "kill it first, or wait for it to finish" >&2
    exit 1
fi
S="${SCRATCH:-$(mktemp -d disk-images/rig232.XXXXXX)}"
echo "scratch: $S"
killall -9 qemu-system-x86_64 2>/dev/null || true
sleep 1

BRIDGE_ISO="$B/alpine-hype-232-bridge.iso"
ADDONS_ISO="$B/hype-additions.iso"
[ -f "$BRIDGE_ISO" ] || { echo "missing $BRIDGE_ISO -- run linux/make-bridge-iso.sh first" >&2; exit 1; }
[ -f "$ADDONS_ISO" ] || { echo "missing $ADDONS_ISO -- run build-additions-iso.sh first" >&2; exit 1; }

dd if=/dev/zero of="$S"/vm0.img bs=1M count=2048 conv=fsync status=none

build_esp() { # $1 = cfg, $2 = out-esp, $3 = preserve-vm0 (1 = copy in existing vm0.img)
    local cfg="$1" esp="$2" preserve="$3"
    if [ "$preserve" = "1" ]; then
        mcopy -o -i "$esp@@1M" ::/hype/disks/vm0.img "$S"/vm0.img
    fi
    rm -f "$esp"
    dd if=/dev/zero of="$esp" bs=1M count=4200 conv=fsync status=none
    sfdisk --label gpt -q "$esp" <<SFDISK
2048,,U
SFDISK
    mformat -i "$esp@@1M" -F ::
    mmd -i "$esp@@1M" ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/hype ::/hype/disks
    mcopy -i "$esp@@1M" build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
    mcopy -i "$esp@@1M" fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
    mcopy -i "$esp@@1M" "$BRIDGE_ISO" ::/iso/bridge.iso
    mcopy -i "$esp@@1M" "$ADDONS_ISO" ::/iso/addons.iso
    mcopy -i "$esp@@1M" "$S"/vm0.img ::/hype/disks/vm0.img
    mcopy -i "$esp@@1M" "$cfg" ::/hype.cfg
    true
}

# Which host disk front-end the ESP is attached through.
#
# Default stays AHCI, because that is what hype's own host AHCI driver is
# exercised by here and switching it silently would quietly stop testing that.
#
# HOST_DISK=nvme is the escape hatch for #730: qemu-10.2.2 segfaults in its OWN
# ahci_commit_buf during a DMA read, which kills a whole 20-minute boot and
# looks like a firmware hang from the outside (73-byte log, qemu at 99% CPU).
# It is intermittent, so a retry is legitimate -- but when it keeps landing,
# NVMe sidesteps the crashing code entirely and hype enumerates it fine (#519).
case "${HOST_DISK:-ahci}" in
    nvme)
        HOST_DISK_ARGS='-drive format=raw,file=PLACEHOLDER,if=none,id=d0 -device nvme,drive=d0,serial=HYPEESPDISK,bootindex=0'
        ;;
    *)
        HOST_DISK_ARGS='-device ich9-ahci,id=ahci -drive format=raw,file=PLACEHOLDER,if=none,id=d0 -device ide-hd,drive=d0,bus=ahci.0,serial=HYPEESPDISK,bootindex=0'
        ;;
esac

run_qemu() { # $1 = esp, $2 = log, $3 = seconds
    cp /usr/share/edk2/ovmf/OVMF_VARS.fd "$S"/VARS.fd
    timeout "$3" qemu-system-x86_64 -machine q35 -m 4096 -nodefaults \
      -accel kvm -cpu host -smp 2 \
      -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
      -drive if=pflash,format=raw,file="$S"/VARS.fd \
      ${HOST_DISK_ARGS//PLACEHOLDER/$1} \
      -serial "file:$2" -display none -vga none || true
}

echo "=== boot 1: unattended Alpine install via the SEPARATE additions ISO ==="
build_esp "$HERE/hype-install.cfg" "$S"/esp.img 0
run_qemu "$S"/esp.img "$S"/boot1.log 1200 &
QPID=$!
for i in $(seq 1 235); do
    sleep 5
    grep -aq "HYPE232: INSTALL SUCCEEDED\|HYPE232: INSTALL FAILED" "$S"/boot1.log 2>/dev/null && break
done
sleep 5
killall qemu-system-x86_64 2>/dev/null || true
wait $QPID 2>/dev/null || true
grep -a "HYPE232-BRIDGE:\|HYPE232:.*INSTALL" "$S"/boot1.log | tail -10
grep -aq "HYPE232: INSTALL SUCCEEDED" "$S"/boot1.log || { echo "FAIL: unattended install did not succeed"; exit 1; }
echo "PASS: boot 1 -- unattended install via the separate additions ISO succeeded"

echo "=== host reboot: fresh qemu process, boot = disk, no CDs attached ==="
killall -9 qemu-system-x86_64 2>/dev/null || true
sleep 2

echo "=== boot 2: boot = disk, no installer media at all ==="
build_esp "$HERE/hype-boot2.cfg" "$S"/esp.img 1
run_qemu "$S"/esp.img "$S"/boot2.log 300 &
QPID=$!
for i in $(seq 1 55); do
    sleep 5
    grep -aq "hype-guest login:" "$S"/boot2.log 2>/dev/null && break
done
sleep 5
killall qemu-system-x86_64 2>/dev/null || true
wait $QPID 2>/dev/null || true
grep -aq "hype-guest login:" "$S"/boot2.log || { echo "FAIL: installed disk did not boot to login with no CDs attached"; exit 1; }
echo "PASS: boot 2 -- installed disk boots to login with no CDs attached at all"

echo "ALL PASS: #232 -- unattended install via a SEPARATE hype-additions ISO," \
     "then a clean boot of the installed disk with no CDs attached"
