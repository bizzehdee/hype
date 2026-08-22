# A live/diskless guest image can never prove disk persistence — use a second, guest-formatted disk

**Tickets:** #687 (USB-SATA exFAT pass, 2026-08-22).

## What happened

Testing that a guest's write to its virtual disk survives an ungraceful
power-off, the first attempt wrote a marker file to `/root/marker.txt` inside
an Alpine guest booted from a pre-built disk image
(`tools/make-guest-disk-from-iso.sh`'s own artifact), forced power off, cold
booted again, and found the marker gone.

That is not evidence hype's storage stack lost the write. Alpine's live/
diskless boot mode mounts its root filesystem as a RAM-only tmpfs overlay over
a squashfs — **nothing written to a plain file there survives ANY reboot, on
any hypervisor, by that image's own design.** `tools/make-guest-disk-from-
iso.sh`'s own header already says as much ("the guest runs Alpine's live/
diskless mode off its own disk... not enough for anything needing a
partitioned persistent install") — it was read, but the implication for a
*persistence* test specifically wasn't drawn out until after a wasted boot
cycle.

The real disk-backend write path was already active during that same boot
(`BLK WRITE count=127 sectors=174` was logged), just not through anything
guest-userspace-visible in a way this test could check.

## The lesson

- **Never test guest-write persistence against a live/diskless boot image's
  own root filesystem.** Its root is not real storage from the guest's point
  of view, independent of whether the underlying virtual disk backend is
  correct.
- Attach a **second disk** and have the guest partition/format it itself
  (`fdisk` + `mkfs.vfat`/`mkfs.ext4`) before writing a marker to it. A disk the
  guest formats fresh is unambiguously real storage, not subject to any
  live-boot overlay.
- Independently confirm the write at the **host** level too, not just by
  trusting the guest's own read-back: `grep -a` / `strings` the raw bytes of
  the backing file for the marker text. This is stronger evidence than a
  guest-reported `cat` — it doesn't depend on the guest's filesystem driver
  being correct, only on the bytes actually landing in the file.
- The same "no network on this machine" constraint that motivated the
  pre-built live image in the first place (see the WiFi-only / no-network
  finding below) is exactly why a real `setup-alpine` install — which would
  have a persistent root — wasn't available either; the second-disk approach
  is the correct workaround for both problems at once, not just the
  persistence one.

## Related: no network means no in-guest package install

The same session hit `setup-alpine`'s `ERROR unable to select packages
dosfstools grub-efi` on a WiFi-only laptop: hype has no WiFi host driver, so
a VM with `net_mode = nat` has no route to a package mirror on that hardware
regardless of guest OS config. Recognize this class of failure (an installer
step needing network, on a machine hype can't get network on) and switch to a
pre-built-image approach (build and control-boot the image on a networked
host, then copy the finished artifact onto the target media) rather than
trying to configure networking that cannot exist on the box.
