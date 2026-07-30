#!/bin/bash
# Boot the INSTALLED disk: no CD at all, and FRESH OVMF vars so nothing can be
# carried over from the install run. Only the removable fallback path can boot.
B=${HYPE_228_BUILD:-$HOME/Downloads/hype-228-build}
killall -9 qemu-system-x86_64 2>/dev/null; sleep 1
cp -f /usr/share/edk2/ovmf/OVMF_VARS.fd $B/vars_boot.fd
exec qemu-system-x86_64 -machine q35 -accel kvm -cpu host -m 3072 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=$B/vars_boot.fd \
  -drive file=$B/vda.img,if=virtio,format=raw \
  -nographic -serial mon:stdio -no-reboot
