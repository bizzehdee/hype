#!/bin/bash
# Bare QEMU (no hype): iterate the seeded ISO fast. Scratch disk is virtio-blk
# so it lands on /dev/vda, matching the answerfile's DISKOPTS.
B=${HYPE_228_BUILD:-$HOME/Downloads/hype-228-build}
killall -9 qemu-system-x86_64 2>/dev/null
sleep 1
cp -f /usr/share/edk2/ovmf/OVMF_VARS.fd $B/vars.fd
exec qemu-system-x86_64 -machine q35 -accel kvm -cpu host -m 3072 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=$B/vars.fd \
  -drive file=$B/alpine-hype-228.iso,media=cdrom,if=ide \
  -drive file=$B/vda.img,if=virtio,format=raw,cache=unsafe \
  -nographic -serial mon:stdio -no-reboot -boot d
