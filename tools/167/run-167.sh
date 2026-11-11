#!/bin/bash
# #167: install FreeBSD from the stock disc1 ISO through hype, then boot the installed disk.
#
#   tools/167/run-167.sh [install|boot2|both]        (default: both)
#
#   boot 1  boot = installer. tools/167/input-vm0.txt drives bsdinstall's TUI over the serial
#           console to a finished install on \hype\disks\g0.img.
#   boot 2  a fresh QEMU process, boot = disk, no ISO on the ESP. tools/167/input-boot2-vm0.txt
#           waits for the installed system's login prompt.
#
# Same two-phase shape as tools/232/run-232-linux.sh. It does not use tools/run-guest.sh because
# that sizes the ESP from the ISO alone (no room for a multi-GB target disk) and requires an ISO
# argument, which boot 2 must not have.
#
# Knobs: SCRATCH (work dir, kept), DISK_MB (target disk size), SECS1/SECS2 (per-boot budget),
# INPUT1/INPUT2 (override the input scripts, for probing one screen at a time).
set -e
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
. ./tools/qemu-env.sh
HERE=tools/167
PHASE="${1:-both}"
ISO="${ISO:-disk-images/FreeBSD-15.0-RELEASE-amd64-disc1.iso}"
S="${SCRATCH:-disk-images/rig167}"
DISK_MB="${DISK_MB:-4000}"   # just under FAT32's 4 GiB file limit
SECS1="${SECS1:-1800}"
SECS2="${SECS2:-420}"
INPUT1="${INPUT1:-$HERE/input-vm0.txt}"
INPUT2="${INPUT2:-$HERE/input-boot2-vm0.txt}"
OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/ovmf/OVMF_CODE.fd}"
OVMF_VARS="${OVMF_VARS:-/usr/share/edk2/ovmf/OVMF_VARS.fd}"
[ -f build/hype.efi ] || { echo "build/hype.efi missing -- run make all" >&2; exit 1; }
mkdir -p "$S"

# A script hype refuses to arm is silent -- the guest boots and nothing drives it.
tools/check-input-scripts.sh "$INPUT1" >/dev/null || { echo "input script rejected: $INPUT1" >&2; exit 1; }

build_esp() { # $1 = cfg, $2 = input script, $3 = with-iso (1/0)
    local esp="$S/esp.img" mb=$(( DISK_MB + 160 ))
    [ "$3" = 1 ] && mb=$(( mb + $(stat -c%s "$ISO") / 1048576 + 1 ))
    rm -f "$esp"
    # Fully allocated, never sparse -- see tools/run-guest.sh build_esp_file().
    fallocate -l "${mb}M" "$esp"
    sfdisk --label gpt -q "$esp" >/dev/null <<SFDISK
2048,,U
SFDISK
    mformat -i "$esp@@1M" -F ::
    mmd -i "$esp@@1M" ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/hype ::/hype/disks ::/input
    mcopy -i "$esp@@1M" build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
    mcopy -i "$esp@@1M" fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
    [ "$3" = 1 ] && mcopy -i "$esp@@1M" "$ISO" ::/iso/test.iso
    mcopy -i "$esp@@1M" "$S/g0.img" ::/hype/disks/g0.img
    mcopy -i "$esp@@1M" "$1" ::/hype.cfg
    mcopy -i "$esp@@1M" "$2" ::/input/vm0.txt
    sync "$esp"
    local have
    have=$(mdir -i "$esp@@1M" ::/hype/disks 2>/dev/null | awk '/g0 *img/ {print $3}')
    [ "$have" = "$(stat -c%s "$S/g0.img")" ] || { echo "ESP verify FAILED: g0.img is $have bytes" >&2; exit 1; }
}

# Boot the ESP until the input script reaches a verdict or the budget runs out. Kills only its own
# QEMU: other rigs may be running on this machine.
run_qemu() { # $1 = log, $2 = seconds
    cp -f "$OVMF_VARS" "$S/vars.fd"
    rm -f "$1"
    "$QEMU" -machine q35 -m 8192 -nodefaults -accel kvm -cpu host -smp 4 \
      -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
      -drive if=pflash,format=raw,file="$S/vars.fd" \
      -drive format=raw,file="$S/esp.img" \
      -serial "file:$1" -display none -vga std 2>"$1.stderr" &
    local qpid=$! i
    for i in $(seq "$2"); do
        kill -0 $qpid 2>/dev/null || break
        grep -aqE "SCRIPT vm0: (PASS|FAIL)" "$1" 2>/dev/null && break
        sleep 1
    done
    # The verdict line overtakes the console text: hype logs guest serial ~25 s behind at this
    # volume (LOGHEALTH behind=~290 kB), so stopping at once truncates the screens that led to it.
    sleep "${POST_VERDICT_SECS:-30}"
    kill $qpid 2>/dev/null || true
    for i in 1 2 3 4 5; do kill -0 $qpid 2>/dev/null || break; sleep 1; done
    kill -9 $qpid 2>/dev/null || true
    wait $qpid 2>/dev/null || true
    grep -aq "^hype: build" "$1" || { echo "INVALID: no hype banner in $1 -- the firmware never launched hype" >&2; exit 2; }
    grep -a "SCRIPT vm0:" "$1" | grep -av SCRIPTFEED | tail -4
}

if [ "$PHASE" = install ] || [ "$PHASE" = both ]; then
    echo "=== boot 1: bsdinstall, driven by $INPUT1 ==="
    rm -f "$S/g0.img"
    fallocate -l "${DISK_MB}M" "$S/g0.img"
    build_esp "$HERE/hype-install.cfg" "$INPUT1" 1
    run_qemu "$S/boot1.log" "$SECS1"
    grep -aq "SCRIPT vm0: PASS" "$S/boot1.log" || { echo "FAIL: boot 1 -- install script did not pass ($S/boot1.log)"; exit 1; }
    echo "PASS: boot 1"
    # The installed disk lives inside the ESP's FAT; pull it out before the ESP is rebuilt.
    mcopy -o -i "$S/esp.img@@1M" ::/hype/disks/g0.img "$S/g0.img"
fi

if [ "$PHASE" = boot2 ] || [ "$PHASE" = both ]; then
    echo "=== boot 2: boot = disk, no installer media ==="
    [ -f "$S/g0.img" ] || { echo "no $S/g0.img -- run the install phase first" >&2; exit 1; }
    tools/check-input-scripts.sh "$INPUT2" >/dev/null || { echo "input script rejected: $INPUT2" >&2; exit 1; }
    build_esp "$HERE/hype-boot2.cfg" "$INPUT2" 0
    run_qemu "$S/boot2.log" "$SECS2"
    grep -aq "SCRIPT vm0: PASS" "$S/boot2.log" || { echo "FAIL: boot 2 -- installed disk did not reach login ($S/boot2.log)"; exit 1; }
    echo "PASS: boot 2"
fi
echo "ALL PASS: #167 ($PHASE)"
