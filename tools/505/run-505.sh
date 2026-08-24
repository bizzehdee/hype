#!/bin/bash
# #505: the full bar. Boot 1: QMP types `mkdisk HYPE505TGT \disks\vm.raw 1 raw` at the
# dashboard; hype creates a fully-allocated 1 GiB RAW image on the SECOND SATA disk's FAT32
# volume, pumped from the dispatch loop with progress and tail-sector verified. Host-side: du
# --apparent-size vs du and filefrag -v on the extracted file confirm no hole. Boot 2: a guest
# attached to it reports a 1 GiB disk and a write persists.
set -e
cd "$(git rev-parse --show-toplevel)"
# DISK, never tmpfs: this rig moves >2.5 GiB of images, and /tmp is RAM-backed (the standing
# never-tmpfs rule -- quota deaths and stolen VM RAM).
S="${SCRATCH:-$(mktemp -d /mnt/data/dev/hype/disk-images/rig505.XXXXXX)}"
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
      -device ide-hd,drive=d1,bus=ahci.1,serial=HYPE505TGT \
      "${qmp[@]}" \
      -serial "file:$2" -display none -vga none || true
}

echo "=== boot 1: mkdisk raw on the second disk (serial HYPE505TGT) ==="
build_esp tools/505/hype-boot1.cfg "$S"/esp1.img
run_qemu "$S"/esp1.img "$S"/boot1.log 1200 "$S"/qmp.sock &
QPID=$!
( sleep 45; python3 tools/qmp-sendkeys.py "$S"/qmp.sock "$(cat tools/505/sendkeys.txt)" > "$S"/keys.log 2>&1 ) &
for i in $(seq 1 230); do
    sleep 5
    LC_ALL=C grep -aq "MKDISK: DONE\|MKDISK: FAILED\|mkdisk: WRITE FAILED\|tail readback FAILED" \
        "$S"/boot1.log 2>/dev/null && break
done
killall qemu-system-x86_64 2>/dev/null || true
wait $QPID 2>/dev/null || true
LC_ALL=C grep -a "TERMCMD.*mkdisk\|MKDISK" "$S"/boot1.log | head -6
LC_ALL=C grep -aq "MKDISK: DONE" "$S"/boot1.log || { echo "FAIL: mkdisk did not finish"; exit 1; }

echo "=== host-side: allocation check on the created file ==="
mcopy -i "$S"/target.img@@1M ::/disks/vm.raw "$S"/vm.raw -n
sync
APPARENT=$(du -b --apparent-size "$S"/vm.raw | cut -f1)
ACTUAL=$(du -b "$S"/vm.raw | cut -f1)
echo "apparent=$APPARENT actual=$ACTUAL"
[ "$APPARENT" = "$ACTUAL" ] || { echo "FAIL: du apparent != actual, image is not fully allocated"; exit 1; }
[ "$APPARENT" -eq 1073741824 ] || { echo "FAIL: not 1 GiB (got $APPARENT)"; exit 1; }
filefrag -v "$S"/vm.raw | tail -5
filefrag -v "$S"/vm.raw | grep -qi "hole" && { echo "FAIL: filefrag reports a hole"; exit 1; }
echo "PASS: fully allocated, no hole, 1 GiB"

echo "=== boot 2: guest attach + write persists (same image, cfg swapped in place) ==="
mcopy -o -i "$S"/esp1.img@@1M tools/505/hype-boot2.cfg ::/hype.cfg
mcopy -o -i "$S"/esp1.img@@1M tools/505/raw-vm0.txt ::/input/vm0.txt
run_qemu "$S"/esp1.img "$S"/boot2.log 600
LC_ALL=C grep -a "QSIZE\|SCRIPT vm0: PASS\|SCRIPT vm0: FAIL" "$S"/boot2.log | head -6
LC_ALL=C grep -aq "SCRIPT vm0: PASS" "$S"/boot2.log || { echo "FAIL: guest leg"; exit 1; }
echo "=== marker visible on the host-extracted raw file too ==="
mcopy -i "$S"/target.img@@1M ::/disks/vm.raw "$S"/vm2.raw -n
MARK=$(dd if="$S"/vm2.raw bs=512 skip=2048 count=1 2>/dev/null | head -c 13)
[ "$MARK" = "HYPE505MARKER" ] || { echo "FAIL: marker not visible in extracted raw file (got '$MARK')"; exit 1; }
echo "ALL PASS: raw created (progress-pumped, tail-verified), fully allocated, 1 GiB visible, guest write persists"
