#!/bin/bash
# #228: offline apk repo for the seeded unattended Alpine install ISO.
# Gotchas encoded here (learned the hard way, see memory/project_228_qemu_install):
#  - full dep closure: `apk add` into a throwaway root, then `apk fetch` the
#    resulting installed set BY NAME. `apk fetch --recursive` misses install_if deps.
#  - the repo needs BOTH x86_64/ and noarch/ subdirs: apk fetches A:noarch
#    packages from <repo>/noarch/, not from <repo>/x86_64/.
set -euo pipefail
# Every image -- base ISOs and everything built from them -- lives in one place:
# <repo>/disk-images, gitignored. Kept off /tmp deliberately, because /tmp is a
# tmpfs and a multi-GB image there eats the RAM a guest's own -m allocation needs.
HYPE_DISK_IMAGES=${HYPE_DISK_IMAGES:-$(cd "$(dirname "$0")/../.." && pwd)/disk-images}
B=${HYPE_228_BUILD:-$HYPE_DISK_IMAGES/hype-228-build}
REPO=$B/apks-hype
rm -rf "$REPO"; mkdir -p "$REPO/x86_64" "$REPO/noarch"

PKGS="alpine-base alpine-conf busybox openrc linux-lts linux-firmware-none mkinitfs grub grub-efi efibootmgr e2fsprogs e2fsprogs-extra dosfstools util-linux sfdisk openssl ca-certificates lddtree ncurses-terminfo-base kmod blkid findmnt doas nano openssh chrony tzdata kbd-bkeymaps mdev-conf busybox-mdev-openrc alpine-keys"

podman run --rm -v "$REPO":/out:z alpine:3.21 sh -euc "
  MIRROR=https://dl-cdn.alpinelinux.org/alpine/v3.21
  mkdir -p /troot/etc/apk/keys
  cp -a /etc/apk/keys/* /troot/etc/apk/keys/ 2>/dev/null || true
  # Resolve the FULL closure by actually installing into a throwaway root.
  apk add --initdb --root /troot --arch x86_64 --no-cache \
      --repository \$MIRROR/main --repository \$MIRROR/community $PKGS
  # Fetch the exact installed set BY NAME -> canonical filenames apk will look for.
  apk info --root /troot | sort -u > /tmp/names
  echo \"closure: \$(wc -l < /tmp/names) packages\"
  mkdir -p /fetch
  apk fetch --no-cache --root /troot --arch x86_64 \
      --repository \$MIRROR/main --repository \$MIRROR/community \
      -o /fetch \$(cat /tmp/names)
  # Layout: apk resolves a repo as <repo>/<arch>/APKINDEX.tar.gz and reads ONLY
  # that index, so EVERY package -- including arch=noarch ones -- must be listed
  # and present in x86_64/. (Alpine's own mirrors do exactly this; there is no
  # separate noarch/ dir on a mirror.) noarch/ is kept as a small extra copy of
  # just the noarch subset, harmless and covers the other reading of the layout.
  cd /fetch
  cp *.apk /out/x86_64/
  for f in *.apk; do
    a=\$(tar -xzOf \"\$f\" .PKGINFO 2>/dev/null | sed -n 's/^arch = //p' | head -1)
    case \"\$a\" in noarch) cp \"\$f\" /out/noarch/ ;; esac
  done
  for d in x86_64 noarch; do
    ( cd /out/\$d && apk index -o APKINDEX.tar.gz *.apk 2>/dev/null || true )
  done
  echo \"x86_64: \$(ls /out/x86_64/*.apk 2>/dev/null | wc -l)  noarch: \$(ls /out/noarch/*.apk 2>/dev/null | wc -l)\"
"
du -sh "$REPO"
