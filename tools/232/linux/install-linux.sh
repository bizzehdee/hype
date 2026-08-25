#!/bin/sh
# #232: unattended Linux (Alpine) install driven by a SEPARATE hype-additions
# medium, generalizing #228's proven combined-ISO recipe (tools/228/autoinstall.start).
#
# Runs from the OS installer's live boot at the `local` runlevel (an apkovl's
# etc/local.d/*.start, or equivalent). Everything is echoed to the console
# because hype relays the guest's ttyS0 into \HYPEFULL.LOG -- that log is the
# only telemetry a real-hardware run produces.
#
# OPEN ITEM (plan.md decision 70, tools/232/README.md): Alpine's live/install
# boot has no built-in "scan every attached medium for an answer file"
# behavior the way FreeBSD's installerconfig or Windows' autounattend.xml do.
# This script still has to be REACHED somehow -- either a small custom apkovl
# on the boot medium that execs it (closer to #228, but the apkovl itself can
# be tiny and generic rather than remastering the whole ISO), or Alpine's own
# `alpine_repo=`/`apkovl=` boot parameters pointed at this additions medium
# directly. Not resolved here; this script is what runs once one of those
# gets it invoked, reading everything else it needs from the additions ISO.
exec >/dev/console 2>&1
set -x
say() { echo "### HYPE232: $*"; }

say "install-linux.sh starting"

# SAFETY, unchanged from #228: setup-alpine copies /etc onto the target, so
# without a guard the installed system re-runs the installer on every boot --
# and hype keeps CDs attached, so it would find the repo again and re-wipe the
# disk in a loop.
rootfs_type=$(awk '$2 == "/" { print $3; exit }' /proc/mounts)
case "$rootfs_type" in
    tmpfs|overlay|overlayfs|squashfs|rootfs) : ;;
    *)
        say "REFUSING TO RUN: root filesystem is '$rootfs_type', not a live boot."
        exit 0
        ;;
esac
if [ -f /etc/hype-232-installed ]; then
    say "REFUSING TO RUN: /etc/hype-232-installed marker present"
    exit 0
fi
say "safety checks passed (live boot, rootfs=$rootfs_type)"

# modloop, unchanged from #228: without it /lib/modules is empty and nothing
# is loadable -- the vfat ESP mount fails 'codepage cp437 not found', and
# ext4 is unavailable for setup-disk's own root mount.
rc-service modloop start || say "modloop start returned $?"
i=0
while [ $i -lt 30 ]; do
    [ -n "$(ls -A /lib/modules 2>/dev/null)" ] && break
    sleep 1; i=$((i+1))
done
say "modloop: /lib/modules='$(ls /lib/modules 2>/dev/null)' after ${i}s"
for m in nls_cp437 nls_iso8859-1 vfat fat ext4 ext2 jbd2 mbcache crc16 \
         virtio_blk virtio_pci virtio_scsi sd_mod; do
    if modprobe "$m" 2>/dev/null; then say "modprobe $m ok"; else say "modprobe $m failed"; fi
done
if ! grep -qw ext4 /proc/filesystems; then
    rc-service modloop start 2>&1
    modprobe ext4 2>&1
fi
grep -qw ext4 /proc/filesystems \
    && say "ext4 REGISTERED" || say "FATAL ext4 still unavailable -- setup-disk WILL fail"

# setup-alpine re-enters the default runlevel partway through, stopping
# modloop and taking /lib/modules with it (#228). Keep a watchdog.
KVER=$(ls /lib/modules 2>/dev/null | head -1)
( set +x; exec >/dev/null 2>&1
  while :; do
      [ -n "$KVER" ] && [ -d "/lib/modules/$KVER/kernel" ] || rc-service modloop start >/dev/null 2>&1
      sleep 2
  done
) &
WATCHDOG=$!
say "modloop watchdog started (pid $WATCHDOG, kver=$KVER)"

# Find the SEPARATE additions medium's linux/ tree -- this is #232's actual
# change from #228: the repo no longer lives on the medium that booted.
ADDITIONS=""
for m in /media/sr0 /media/sr1 /media/cdrom /media/cdrom1 /media/vdb /media/usb; do
    [ -d "$m/linux/apks-hype/x86_64" ] && { ADDITIONS="$m/linux"; break; }
done
if [ -z "$ADDITIONS" ]; then
    for d in /media/*/linux; do [ -d "$d/apks-hype/x86_64" ] && { ADDITIONS="$d"; break; }; done
fi
say "additions='$ADDITIONS'"
[ -n "$ADDITIONS" ] || { say "FATAL no hype-additions linux/ tree found on any attached medium"; exit 1; }
REPO="$ADDITIONS/apks-hype"

# The repo is unsigned -- shim apk so setup-disk's OWN internal calls (it
# shells out to apk itself) get --allow-untrusted too, not just our own argv.
mkdir -p /usr/local/sbin
cat > /usr/local/sbin/apk <<'EOA'
#!/bin/sh
exec /sbin/apk --allow-untrusted "$@"
EOA
chmod +x /usr/local/sbin/apk
export PATH=/usr/local/sbin:$PATH

echo "$REPO" > /etc/apk/repositories
apk update && say "apk update ok" || say "apk update rc=$?"

# Full answerfile. Every key must be set -- an unset option prompts, and a
# prompt is an unattended hang with no operator (#228).
HOSTNAME=${HYPE232_HOSTNAME:-hype-guest}
cat > /tmp/answers <<EOF
KEYMAPOPTS="us us"
HOSTNAMEOPTS="-n $HOSTNAME"
DEVDOPTS="mdev"
INTERFACESOPTS="auto lo
iface lo inet loopback
"
DNSOPTS="-d hype 1.1.1.1"
TIMEZONEOPTS="-z UTC"
PROXYOPTS="none"
APKREPOSOPTS="$REPO"
USEROPTS="-a -u -g audio,video,netdev hypeuser"
SSHDOPTS="-c openssh"
NTPOPTS="-c chrony"
DISKOPTS="-m sys /dev/vda"
LBUOPTS="none"
APKCACHEOPTS="none"
EOF
say "answerfile:"; cat /tmp/answers

# Re-wipe guard (#228 GUARD 4, the one that actually matters under hype): hype
# keeps CDs attached, so the live medium can boot again after a successful
# install and would re-wipe the disk on every subsequent boot without this.
target_already_installed() {
    _found=1
    mkdir -p /tmp/probe
    for _p in /dev/vda3 /dev/vda2 /dev/vda1; do
        [ -b "$_p" ] || continue
        if mount -o ro "$_p" /tmp/probe 2>/dev/null; then
            [ -f /tmp/probe/etc/hype-232-installed ] && _found=0
            umount /tmp/probe 2>/dev/null
        fi
        [ $_found -eq 0 ] && break
    done
    return $_found
}
FORCE=no
for _m in /media/sr0 /media/sr1 /media/*; do
    [ -f "$_m/HYPE232_FORCE" ] && FORCE=yes
done
if target_already_installed && [ "$FORCE" != yes ]; then
    say "REFUSING TO INSTALL: /dev/vda already carries a completed #232 install"
    say "override with a file named HYPE232_FORCE on any attached medium"
    kill "$WATCHDOG" 2>/dev/null
    sync; sleep 2; poweroff -f
    exit 0
fi
say "target check: no existing #232 install found (force=$FORCE) -- proceeding"

export ERASE_DISKS=/dev/vda
export BOOTLOADER=grub
say "running setup-alpine"
timeout 900 setup-alpine -e -f /tmp/answers
rc=$?
say "setup-alpine rc=$rc"

# Post-install: make the target bootable under the guest's OVMF (no NVRAM
# entry -- only the removable fallback path is tried), and give the installed
# initramfs a driver set that boots wherever it ends up, not just under hype's
# own virtual disk (#226's own lesson, folded into #228's fix).
ROOT=/mnt
ROOTDEV=$(blkid | sed -n 's|^\(/dev/[a-z0-9]*\): .*TYPE="ext4".*|\1|p' | head -1)
ESPDEV=$(blkid | sed -n 's|^\(/dev/[a-z0-9]*\): .*TYPE="vfat".*|\1|p' | head -1)
say "post-install: rootdev='$ROOTDEV' espdev='$ESPDEV'"
installed=no
existing=$(awk -v d="$ROOTDEV" '$1 == d { print $2; exit }' /proc/mounts)
if [ -n "$existing" ]; then ROOT="$existing"; fi
if [ -n "$ROOTDEV" ]; then
    mkdir -p "$ROOT"
    if [ -n "$existing" ] || mount -t ext4 "$ROOTDEV" "$ROOT" 2>&1; then
        [ -d "$ROOT/etc" ] && [ -d "$ROOT/boot" ] && installed=yes
    fi
fi

if [ "$installed" = yes ]; then
    say "target rootfs verified on $ROOTDEV -- fixing bootloader + console + initramfs"
    [ -n "$ESPDEV" ] && { mkdir -p "$ROOT/boot/efi"; mount -t vfat "$ESPDEV" "$ROOT/boot/efi" 2>&1; }
    mount --bind /dev  "$ROOT/dev"  2>/dev/null
    mount --bind /proc "$ROOT/proc" 2>/dev/null
    mount --bind /sys  "$ROOT/sys"  2>/dev/null
    cat > "$ROOT/etc/default/grub" <<'EOG'
GRUB_DISTRIBUTOR="Alpine"
GRUB_TIMEOUT=1
GRUB_DISABLE_SUBMENU=y
GRUB_DISABLE_RECOVERY=true
GRUB_TERMINAL="serial console"
GRUB_SERIAL_COMMAND="serial --unit=0 --speed=115200"
GRUB_CMDLINE_LINUX_DEFAULT="console=tty0 console=ttyS0,115200 rootfstype=ext4"
EOG
    mkdir -p "$ROOT/etc/mkinitfs"
    # Alpine's OWN sys-install default, verbatim -- do NOT hand-curate this
    # against hype's virtual disk. #226 found the installed system unbootable
    # on real SATA hardware when this list was trimmed to just what hype's
    # own virtio disk needed.
    cat > "$ROOT/etc/mkinitfs/mkinitfs.conf" <<'EOM'
features="ata base cdrom ext4 keymap kms mmc nvme raid scsi usb virtio"
EOM
    chroot "$ROOT" /bin/sh -euxc '
        export PATH=/usr/local/sbin:$PATH
        KV=$(ls /lib/modules | head -1)
        mkinitfs -c /etc/mkinitfs/mkinitfs.conf "$KV"
        grub-install --target=x86_64-efi --efi-directory=/boot/efi --removable --no-nvram
        grub-mkconfig -o /boot/grub/grub.cfg
    ' 2>&1
    say "post-install chroot rc=$?"
    rm -f "$ROOT/etc/local.d/install-linux.start"
    : > "$ROOT/etc/hype-232-installed"
    sync
    umount "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/boot/efi" 2>/dev/null
    umount "$ROOT" 2>/dev/null
    sync
fi

if [ "$rc" -eq 0 ] && [ "$installed" = yes ]; then
    say "INSTALL SUCCEEDED (setup-alpine rc=0, rootfs + bootloader written to $ROOTDEV)"
else
    say "INSTALL FAILED (setup-alpine rc=$rc, target rootfs verified=$installed)"
fi
kill "$WATCHDOG" 2>/dev/null
say "powering off"
sync; sleep 2
poweroff -f
