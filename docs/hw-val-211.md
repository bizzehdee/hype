# HW-VAL runbook: #211 — raw-file virtual disk on a real disk-backed FAT32/exFAT volume

Status: **ready to run** (2026-08-12, build `c493e5c`). Code prerequisites
#198/#199 are closed; the filesystem-refactor series (#292→#385) landed on
top, so this run also smoke-tests the refactored stack on real silicon.

Media: `release/hype-usb.img` (dd/Rufus/balenaEtcher) or the
`release/usb/` tree onto an already-FAT32 stick. Rebuild with
`tools/make-usb-package.sh` after any code change.

## Safety (standing, non-negotiable)

- Physical targets are selected **by serial**, never by index.
- The Intel box's only internal NVMe (BIWIN NV3500) is a BitLocker Windows
  install: **never a target**.
- thor's `nvme1n1` (WD Blue SN5000) is dying and lies about its own
  contents: **never a target**.
- The AMD laptop's spare scratch NVMe is the only sanctioned write target.

## Procedure (cold boot; run on AMD, repeat on Intel where possible)

1. On a real OS, create a fully-allocated raw disk image on the scratch
   disk's FAT32 volume with `tools/make-disk-image.sh`. Repeat on an exFAT
   volume (second partition or reformat between passes).
2. Configure the image as the guest's `file:` target in `hype.cfg` on the
   boot stick.
3. Cold boot hype. Let the guest write recognisable data to its disk
   (marker file or small filesystem). Use Force power off for one pass —
   the persistence guarantee includes ungraceful teardown.
4. Cold boot again. The guest must see its own writes.
5. From a real OS: `fsck.vfat -n` / `fsck.exfat -n` on the volume must be
   clean, and the backing file's bytes must have changed exactly where the
   guest wrote (compare against a pre-run copy of the image).

## Evidence to attach to the ticket

- `hype.log` + `vmN.log` off the stick (`LC_ALL=C grep -a` when mining).
- Photo of the frozen GOP screen on any failure.
- fsck output, build hash, host model, volume geometry.
