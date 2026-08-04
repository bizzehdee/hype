#!/bin/bash
# Bare QEMU (no hype): iterate the seeded ISO fast. Scratch disk is virtio-blk
# so it lands on /dev/vda, matching the answerfile's DISKOPTS.
# Every image -- base ISOs and everything built from them -- lives in one place:
# <repo>/disk-images, gitignored. Kept off /tmp deliberately, because /tmp is a
# tmpfs and a multi-GB image there eats the RAM a guest's own -m allocation needs.
HYPE_DISK_IMAGES=${HYPE_DISK_IMAGES:-$(cd "$(dirname "$0")/../.." && pwd)/disk-images}
B=${HYPE_228_BUILD:-$HYPE_DISK_IMAGES/hype-228-build}
killall -9 qemu-system-x86_64 2>/dev/null
sleep 1
cp -f /usr/share/edk2/ovmf/OVMF_VARS.fd $B/vars.fd
exec qemu-system-x86_64 -machine q35 -accel kvm -cpu host -m 3072 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=$B/vars.fd \
  -drive file=$B/alpine-hype-228.iso,media=cdrom,if=ide \
  -drive file=$B/vda.img,if=virtio,format=raw,cache=unsafe \
  -nographic -serial mon:stdio -no-reboot -boot d
