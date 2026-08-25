#!/bin/bash
# #120 (M9-5): reboot the HOST into hype.efi again, and boot an already-installed guest disk --
# not just a fresh installer. Boot 1 installs Alpine unattended (setup-alpine answerfile) to a
# real persistent disk; the qemu process is then killed entirely and a FRESH one launched for
# boot 2 (a real host power-cycle equivalent), configured with `boot = disk` and no installer
# media at all, which must load the disk's OWN bootloader straight to a login prompt.
#
# Also proves #716 (the date-based build version string): both boots' serial logs are checked
# for the "hype: version YYYY.M.D[-tag] (#commit)" startup-banner line in the right shape.
set -e
export LC_ALL=C
cd "$(git rev-parse --show-toplevel)"
S="${SCRATCH:-$(mktemp -d /mnt/data/dev/hype/disk-images/rig120.XXXXXX)}"
echo "scratch: $S"
killall -9 qemu-system-x86_64 2>/dev/null || true
sleep 1

# The guest's own disk, fully preallocated (real zero bytes -- #90's own established recipe;
# blk_image requires this, and fallocate/truncate leave exactly the traps make-disk-image.sh
# documents on some filesystems).
dd if=/dev/zero of="$S"/vm0.img bs=1M count=1536 conv=fsync status=none

build_esp() { # $1 = cfg, $2 = input-script, $3 = out-esp, $4 = preserve-vm0 (1 = copy in existing vm0.img)
    local cfg="$1" script="$2" esp="$3" preserve="$4"
    if [ "$preserve" = "1" ]; then
        # boot 2: pull the disk OUT of boot 1's esp before rebuilding, so it survives untouched.
        mcopy -o -i "$esp@@1M" ::/hype/disks/vm0.img "$S"/vm0.img
    fi
    rm -f "$esp"
    dd if=/dev/zero of="$esp" bs=1M count=2600 conv=fsync status=none
    sfdisk --label gpt -q "$esp" <<SFDISK
2048,,U
SFDISK
    mformat -i "$esp@@1M" -F ::
    mmd -i "$esp@@1M" ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/input ::/hype ::/hype/disks
    mcopy -i "$esp@@1M" build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
    mcopy -i "$esp@@1M" fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
    mcopy -i "$esp@@1M" disk-images/alpine-hype-dbg.iso ::/iso/test.iso
    mcopy -i "$esp@@1M" "$S"/vm0.img ::/hype/disks/vm0.img
    mcopy -i "$esp@@1M" "$cfg" ::/hype.cfg
    mcopy -i "$esp@@1M" "$script" ::/input/vm0.txt
    true
}

run_qemu() { # $1 = esp, $2 = log, $3 = seconds, $4 = 1 to attach hype's host-facing NIC
    local net=()
    [ "${4:-}" = "1" ] && net=(-netdev user,id=n0 -device e1000,netdev=n0)
    cp /usr/share/edk2/ovmf/OVMF_VARS.fd "$S"/VARS.fd
    timeout "$3" qemu-system-x86_64 -machine q35 -m 4096 -nodefaults \
      -accel kvm -cpu host -smp 4 \
      -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
      -drive if=pflash,format=raw,file="$S"/VARS.fd \
      -device ich9-ahci,id=ahci \
      -drive format=raw,file="$1",if=none,id=d0 \
      -device ide-hd,drive=d0,bus=ahci.0,serial=HYPEESPDISK,bootindex=0 \
      "${net[@]}" \
      -serial "file:$2" -display none -vga none || true
}

check_version_banner() { # $1 = log, $2 = label
    local line
    # tr -d '\r': the guest serial console's own CRLF line endings, stripped before matching --
    # otherwise the trailing ^M breaks the $ end-anchor below.
    line=$(grep -a "^hype: version " "$1" | head -1 | tr -d '\r')
    echo "[$2] $line"
    [ -n "$line" ] || { echo "FAIL: [$2] no 'hype: version' banner line at all"; exit 1; }
    echo "$line" | grep -qE '^hype: version [0-9]{4}\.[0-9]{1,2}\.[0-9]{1,2}(-[a-z]+)? \(#[0-9a-f]+(-dirty)?\)$' \
        || { echo "FAIL: [$2] banner does not match YYYY.M.D[-tag] (#commit)"; exit 1; }
    echo "[$2] PASS: #716 version banner well-formed"
}

echo "=== boot 1 (host boot #1): unattended install to \\hype\\disks\\vm0.img ==="
build_esp tools/120/hype-boot1.cfg tools/120/install-vm0.txt "$S"/esp.img 0
run_qemu "$S"/esp.img "$S"/boot1.log 1200 1 &
QPID=$!
for i in $(seq 1 230); do
    sleep 5
    grep -aq "SCRIPT vm0: PASS\|SCRIPT vm0: FAIL" "$S"/boot1.log 2>/dev/null && break
done
sleep 5  # let qemu's own buffered serial-log writes flush before the SIGTERM below
killall qemu-system-x86_64 2>/dev/null || true
wait $QPID 2>/dev/null || true
grep -a "SCRIPT vm0: PASS\|SCRIPT vm0: FAIL" "$S"/boot1.log | head -4
grep -aq "SCRIPT vm0: PASS" "$S"/boot1.log || { echo "FAIL: unattended install did not complete"; exit 1; }
check_version_banner "$S"/boot1.log "boot1"
grep -a "MOUNT-\|FALLBACK-COPY-\|BLK-CHECK-" "$S"/boot1.log | tail -6
grep -aq "MOUNT-0" "$S"/boot1.log || { echo "FAIL: the ESP partition never mounted in the live env"; exit 1; }
grep -aq "FALLBACK-COPY-0" "$S"/boot1.log || { echo "FAIL: the UEFI fallback bootloader copy failed -- boot 2 would have nothing to load"; exit 1; }
echo "PASS: install completed, fallback bootloader path confirmed in place"

echo "=== host reboot: a COMPLETELY FRESH qemu process, no state carried except the disk ==="
killall -9 qemu-system-x86_64 2>/dev/null || true
sleep 2

echo "=== boot 2 (host boot #2): boot = disk, no installer media at all ==="
build_esp tools/120/hype-boot2.cfg tools/120/login-vm0.txt "$S"/esp.img 1
run_qemu "$S"/esp.img "$S"/boot2.log 600 &
QPID=$!
for i in $(seq 1 115); do
    sleep 5
    grep -aq "SCRIPT vm0: PASS\|SCRIPT vm0: FAIL" "$S"/boot2.log 2>/dev/null && break
done
sleep 5  # let qemu's own buffered serial-log writes flush before the SIGTERM below
killall qemu-system-x86_64 2>/dev/null || true
wait $QPID 2>/dev/null || true
grep -a "SCRIPT vm0: PASS\|SCRIPT vm0: FAIL" "$S"/boot2.log | head -4
grep -aq "SCRIPT vm0: PASS" "$S"/boot2.log || { echo "FAIL: boot 2 (disk boot) did not reach the installed system"; exit 1; }
check_version_banner "$S"/boot2.log "boot2"
echo "PASS: boot 2 loaded the disk's OWN bootloader, no installer involved (hype-boot2.cfg has no install_media key at all)"

echo "ALL PASS: #120 -- a host reboot (fresh qemu process) boots an already-installed guest" \
     "disk via boot = disk with no installer media, and #716's version banner is well-formed" \
     "on both boots"
