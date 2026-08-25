#!/bin/bash
# #232: offline apk repo for the hype-additions ISO's linux/ tree.
#
# Same recipe #228 proved end to end (see tools/228/build-offline-repo.sh) --
# this is that recipe with two changes: it writes to the additions-ISO layout
# (tools/232/build-additions-iso.sh's linux/apks-hype/, not a combined
# installer-ISO staging tree) and it drops the noarch/ subdir #228 also wrote,
# per the #232 ticket's own correction: apk resolves a repo as
# <repo>/<arch>/APKINDEX.tar.gz and reads ONLY that index, so a noarch/ copy is
# harmless clutter, not a second thing apk actually consults.
set -euo pipefail
HYPE_DISK_IMAGES=${HYPE_DISK_IMAGES:-$(cd "$(dirname "$0")/../../.." && pwd)/disk-images}
OUT=${HYPE_232_LINUX_REPO:-$HYPE_DISK_IMAGES/hype-232-build/linux/apks-hype}
rm -rf "$OUT"; mkdir -p "$OUT/x86_64"

# The full install closure #228 measured, plus kbd-bkeymaps (needed by
# setup-keymap but not pulled in by anything else -- #228's own ticket comment
# flagged this as a silent first-prompt hang if omitted).
PKGS="alpine-base alpine-conf busybox openrc linux-lts linux-firmware-none mkinitfs grub grub-efi efibootmgr e2fsprogs e2fsprogs-extra dosfstools util-linux sfdisk openssl ca-certificates lddtree ncurses-terminfo-base kmod blkid findmnt doas nano openssh chrony tzdata kbd-bkeymaps mdev-conf busybox-mdev-openrc alpine-keys"

podman run --rm -v "$OUT":/out:z alpine:3.21 sh -euc "
  MIRROR=https://dl-cdn.alpinelinux.org/alpine/v3.21
  mkdir -p /troot/etc/apk/keys
  cp -a /etc/apk/keys/* /troot/etc/apk/keys/ 2>/dev/null || true
  # Resolve the FULL closure by actually installing into a throwaway root --
  # 'apk fetch --recursive' misses install_if deps (#228).
  apk add --initdb --root /troot --arch x86_64 --no-cache \
      --repository \$MIRROR/main --repository \$MIRROR/community $PKGS
  apk info --root /troot | sort -u > /tmp/names
  echo \"closure: \$(wc -l < /tmp/names) packages\"
  mkdir -p /fetch
  apk fetch --no-cache --root /troot --arch x86_64 \
      --repository \$MIRROR/main --repository \$MIRROR/community \
      -o /fetch \$(cat /tmp/names)
  cp /fetch/*.apk /out/x86_64/
  ( cd /out/x86_64 && apk index -o APKINDEX.tar.gz *.apk )
  echo \"x86_64: \$(ls /out/x86_64/*.apk | wc -l) packages indexed\"
"
du -sh "$OUT"
