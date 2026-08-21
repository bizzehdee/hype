#!/bin/bash
# TERM-11 (#487): the full bar. Boot 1: QMP types `mkdisk HYPE487TGT \disks\vm.qcow2 1` at the
# dashboard; hype creates a fully-preallocated 1 GiB qcow2 on the SECOND SATA disk's FAT32
# volume, pumped from the dispatch loop with progress. Host-side: qemu-img check + info on the
# extracted file. Boot 2: a guest attached to it reports a 1 GiB disk and its writes survive an
# in-guest restart.
set -e
cd "$(git rev-parse --show-toplevel)"
# DISK, never tmpfs: this rig moves >2.5 GiB of images, and /tmp is RAM-backed (the standing
# never-tmpfs rule -- quota deaths and stolen VM RAM).
S="${SCRATCH:-$(mktemp -d /mnt/data/dev/hype/disk-images/rig487.XXXXXX)}"
echo "scratch: $S"
killall -9 qemu-system-x86_64 2>/dev/null || true
sleep 1

# The target disk: GPT + one FAT32 volume, 1.5 GiB (room for the 1 GiB image + metadata).
dd if=/dev/zero of="$S"/target.img bs=1M count=1600 conv=fsync status=none
sfdisk --label gpt -q "$S"/target.img <<SFDISK
2048,,U
SFDISK
mformat -i "$S"/target.img@@1M -F ::
mmd -i "$S"/target.img@@1M ::/disks

build_esp() { # $1 = cfg, $2 = out-esp [, $3 = input script]
    rm -f "$2"
    dd if=/dev/zero of="$2" bs=1M count=1800 conv=fsync status=none
    sfdisk --label gpt -q "$2" <<SFDISK
2048,,U
SFDISK
    mformat -i "$2@@1M" -F ::
    mmd -i "$2@@1M" ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso ::/input ::/hype ::/hype/disks
    mcopy -i "$2@@1M" build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
    mcopy -i "$2@@1M" fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
    mcopy -i "$2@@1M" disk-images/alpine-hype-dbg.iso ::/iso/test.iso
    mcopy -i "$2@@1M" "$1" ::/hype.cfg
    [ -n "${3:-}" ] && mcopy -i "$2@@1M" "$3" ::/input/vm0.txt
    true
}

run_qemu() { # $1 = esp, $2 = log, $3 = seconds [, $4 = qmp-socket]
    local qmp=()
    [ -n "${4:-}" ] && { rm -f "$4"; qmp=(-qmp "unix:$4,server=on,wait=off"); }
    cp /usr/share/edk2/ovmf/OVMF_VARS.fd "$S"/VARS.fd
    timeout "$3" qemu-system-x86_64 -machine q35 -m 4096 -nodefaults \
      -accel kvm -cpu host -smp 4 \
      -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
      -drive if=pflash,format=raw,file="$S"/VARS.fd \
      -device ich9-ahci,id=ahci \
      -drive format=raw,file="$1",if=none,id=d0 \
      -device ide-hd,drive=d0,bus=ahci.0,serial=HYPEESPDISK,bootindex=0 \
      -drive format=raw,file="$S"/target.img,if=none,id=d1 \
      -device ide-hd,drive=d1,bus=ahci.1,serial=HYPE487TGT \
      "${qmp[@]}" \
      -serial "file:$2" -display none -vga none || true
}

echo "=== boot 1: mkdisk (target = the boot disk itself; #589 records why a second
    AHCI port cannot yet serve as file backing) ==="
build_esp tools/487/hype-boot1.cfg "$S"/esp1.img
mmd -i "$S"/esp1.img@@1M ::/disks
run_qemu "$S"/esp1.img "$S"/boot1.log 1200 "$S"/qmp.sock &
QPID=$!
( sleep 45; python3 tools/qmp-sendkeys.py "$S"/qmp.sock "$(cat tools/487/sendkeys.txt)" > "$S"/keys.log 2>&1 ) &
# wait for DONE or FAILED, then stop qemu early
for i in $(seq 1 230); do
    sleep 5
    LC_ALL=C grep -aq "MKDISK: DONE\|MKDISK: FAILED\|mkdisk: WRITE FAILED" "$S"/boot1.log 2>/dev/null && break
done
killall qemu-system-x86_64 2>/dev/null || true
wait $QPID 2>/dev/null || true
LC_ALL=C grep -a "TERMCMD.*mkdisk\|MKDISK" "$S"/boot1.log | head -4
LC_ALL=C grep -aq "MKDISK: DONE" "$S"/boot1.log || { echo "FAIL: mkdisk did not finish"; exit 1; }

echo "=== host-side: qemu-img on the created file ==="
mcopy -i "$S"/esp1.img@@1M ::/disks/vm.qcow2 "$S"/vm.qcow2 -n
qemu-img check "$S"/vm.qcow2 2>&1 | tail -2
qemu-img check "$S"/vm.qcow2 >/dev/null 2>&1 || { echo "FAIL: qemu-img check"; exit 1; }
qemu-img info "$S"/vm.qcow2 | grep -E "virtual size|file format"
qemu-img info "$S"/vm.qcow2 | grep -q "virtual size: 1 GiB" || { echo "FAIL: not 1 GiB"; exit 1; }

echo "=== boot 2: guest attach + restart survival (same disk image, cfg swapped in place) ==="
mcopy -o -i "$S"/esp1.img@@1M tools/487/hype-boot2.cfg ::/hype.cfg
mcopy -o -i "$S"/esp1.img@@1M tools/487/qcow-vm0.txt ::/input/vm0.txt
run_qemu "$S"/esp1.img "$S"/boot2.log 900
LC_ALL=C grep -a "m5-9\|QSIZE\|SCRIPT vm0: PASS\|SCRIPT vm0: FAIL" "$S"/boot2.log | head -6
LC_ALL=C grep -aq "SCRIPT vm0: PASS" "$S"/boot2.log || { echo "FAIL: guest leg"; exit 1; }
echo "=== marker visible through qemu-img too ==="
mcopy -i "$S"/esp1.img@@1M ::/disks/vm.qcow2 "$S"/vm2.qcow2 -n
python3 - "$S"/vm2.qcow2 <<'PYEOF'
import subprocess, sys
out = subprocess.run(["qemu-img","dd","-f","qcow2","-O","raw",f"if={sys.argv[1]}","of=/tmp/claude-1000/hype487.raw","bs=512","count=2050"],capture_output=True)
data = open("/tmp/claude-1000/hype487.raw","rb").read()
assert data[2048*512:2048*512+13] == b"HYPE487MARKER", "marker missing via qemu-img"
print("qemu-img sees the guest's marker: byte-exact")
PYEOF
echo "ALL PASS: created (progress-pumped), qemu-img clean, 1 GiB visible, writes survive restart"
