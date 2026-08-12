# HW-VAL runbook: #386 — sparse file-backed disks on FAT32, exFAT, ext and NTFS

Status: **code-complete, awaiting hardware** (2026-08-12, build `c493e5c`).
Every writer below is real-image-validated in host harnesses — `fsck.vfat`,
`fsck.exfat`, `e2fsck -f -n` and `ntfsfix -n` all clean after hype's sparse
writes, including a 26-point ext3 crash sweep in which e2fsck genuinely
replays hype's journal. What remains is exactly what QEMU cannot prove:
cold-boot behaviour against real disks and controllers on both vendors.

Blocked by #211's pass (run that first) plus these per-format passes.
Safety rules: as `docs/hw-val-211.md` — targets by serial only.

## Per-format passes (cold boot, both vendors)

### FAT32 (#382)
1. Start with a SMALL backing file; have the guest write far past its EOF.
2. Verify: every intervening cluster allocated, gaps read back as zeros,
   `fsck.vfat -n` clean.

### exFAT (#383)
1. From a real OS, create a short-VDL file (write a prefix, then truncate
   UP) — the kernel driver produces genuine `ValidDataLength < DataLength`.
2. Verify hype reads the uninitialized tail as zeros (never stale bytes).
3. Random growth on a deliberately fragmented volume (NoFatChain must
   materialize a real FAT chain). `fsck.exfat -n` clean.

### ext2 (#384)
1. A truly sparse backing file (holes at direct and indirect levels).
2. Holes read as zeros; guest writes INTO holes; reboot; data present;
   `e2fsck -f -n` clean.

### ext3 / ext4 (#385)
1. Repeat the ext2 pass on journaled volumes (ext4 image built with
   `mkfs.ext4 -O ^metadata_csum,^64bit` — checksummed metadata is refused
   by design, see the #385 close-out).
2. THE INTERRUPTED RUN: cut power mid-write-burst. On the next boot hype
   must REFUSE the volume while the journal is non-empty; `e2fsck -f -y`
   replays; `e2fsck -f -n` is then clean and the data is old-or-new, never
   torn.

### NTFS (#337, in-place only)
1. Backing file on a plain NTFS scratch volume (BY SERIAL — never the
   BitLocker system NVMe; BitLocker is permanently out of scope).
2. Sparse runs read as zeros; in-place guest writes persist across a cold
   reboot; a write aimed at a hole is REFUSED with nothing written; a
   dirty volume (Windows fast startup) is refused.
3. `ntfsfix -n` clean + byte-exact compare through a host ntfs-3g mount.

### Isolation
Two guests, separate backing files on one volume: neither may affect the
other's file or the host filesystem's allocation.

## Evidence to attach to the ticket

Configuration, build hash, host model, filesystem geometry, guest
commands, returned logs (`hype.log` + `vmN.log`, `LC_ALL=C`), file
allocation maps (`debugfs`/`ntfsinfo`/fsck output) for every pass.
