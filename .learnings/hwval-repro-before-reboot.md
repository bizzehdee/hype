# Reproduce a suspected storage-driver bug host-side before spending another physical boot on it

**Tickets:** #696, #697 (USB-SATA ext4/NTFS passes, 2026-08-22).

## What happened

A file-backed disk that worked when its backing file sat on an exFAT
partition failed once the same partition was reformatted ext4 — one disk
resolved but the guest couldn't boot from it, a second disk didn't resolve at
all. The temptation with a real-hardware defect is to keep changing the
config and cold-booting again to narrow it down, which is expensive on a
cold-boot-only machine.

Instead, the relevant resolvers (`core/ext.c`'s `hype_ext_resolve()`,
`core/blk_image.c`'s `hype_blk_image_locate()`) are plain, host-buildable C
with no privileged instructions — exactly the "testable" code the `testing`
skill already requires 90% coverage on. A `mkfs.ext4`'d loopback image with
files injected and a ~30-line harness calling `hype_ext_resolve()` directly
reproduced the resolution failure without touching the physical drive at all.
That eventually ruled OUT `boot/main.c`'s device/partition-scanning
orchestration (the initial suspect, carried over from an earlier session) —
the same code path, given a correctly-built ext4 image, resolved every file
correctly — and instead isolated the real defect to how the TEST image itself
was built, not to any code in `core/` or `boot/main.c`. See "The actual root
cause" below.

The NTFS case needed no such harness at all: reading `fw_1_resolve_on_any_fs()`
directly showed it only ever calls the FAT32/exFAT/ext resolvers — NTFS is not
attempted, period. The error message even names the three filesystems it
tried. One `grep`/read settled it; no reproduction needed once the dispatcher
itself was read instead of assumed.

## The actual root cause (#696)

`hype_ext_resolve()`'s legacy resolver (`map_inode()` / `leaf_entry()` in
`core/ext.c`) refuses any file with an ext4 "unwritten" extent
(`EE_LEN_UNWRITTEN`, the high bit of the on-disk extent length). That refusal
is deliberate, not a bug: `hype_file_map_t` is a flat list of physical
sectors with no zero-fill/hole semantics, and hype reads those sectors
straight off the medium, bypassing the host's ext4 driver entirely. An
unwritten extent has real, allocated blocks whose CONTENTS are unspecified
until something writes them — the "reads as zero" guarantee is enforced by
the ext4 driver at read time, not by anything on the physical medium — so
trusting it under raw sector access could hand a guest garbage instead of
zeros. Refusing is the safe call.

The defect was in `tools/make-disk-image.sh`, which used `fallocate()` as
its fast preallocation path for ext2/3/4 targets. `fallocate()` reserves the
space but leaves it as an unwritten extent — exactly the state
`hype_ext_resolve()` refuses. The script's own `probe_tail_valid()` check
could not catch this because it reads back through the host's ext4 driver,
which — per the paragraph above — transparently returns zeros for unwritten
extents, so the check reported the image fine. This is the SAME class of bug
the script already had a guard for on exFAT (`ValidDataLength` lagging
`DataLength`); it just wasn't extended to ext. Fix: ext2/3/4 now take the
`dd`-based real-zero-write path unconditionally, same as exFAT, so no
unwritten extent is ever created in a hype disk image.

## The lesson

- Before spending a physical boot to narrow down a storage/filesystem defect,
  ask whether the suspected component is plain host-buildable C (it usually
  is — `core/`'s FS resolvers, `blk_image.c`, `blk_backend.c` all avoid
  privileged instructions by design). If so, reproduce it in a throwaway
  loopback image first.
- **`debugfs -w -R "write <src> <dst>"` is UNRELIABLE for injecting a test
  file into an ext2/3/4 image.** It can silently scatter the written blocks
  across the volume — producing a genuinely fragmented/holey extent layout
  that does not match the source file at all — which then makes
  `hype_ext_resolve()`'s "every block must be contiguously mapped" check
  correctly refuse the file, a false-positive "bug" that is actually a
  `debugfs` artifact. Use a real mount instead: `fuse2fs -o fakeroot
  [-o offset=<bytes>] <image> <mountpoint>` (no root needed) plus a plain
  `cp` produces a clean, kernel-realistic extent layout and is the technique
  to use for future ext4-image test-file injection.
- A round-trip test (resolve → reconstruct via the extent map → byte-compare
  against the source) is stronger evidence than "it resolved without an
  error" — it catches boundary-crossing bugs a bare resolve-success check
  would miss.
- A test IMAGE can itself be the source of a false bug signal. Before
  concluding a resolver is broken, check how the file under test actually
  got onto the disk — a "fully allocated" image-creation tool can still leave
  unwritten (fallocate) extents that a host-level readback check cannot see,
  because the host's own filesystem driver papers over exactly the gap being
  tested for.
- When a scan-and-report function's error message names a fixed list (like
  `fw_1_resolve_on_any_fs`'s log line naming exactly the filesystems it
  tries), that list is often literally the whole answer — read the function
  before assuming the failure is data-dependent (fragmentation, size, layout)
  when it might just be a missing code path.
- Two failures that look similar ("file not found on this partition") can
  have completely different root causes at completely different depths (a
  runtime orchestration bug vs. a filesystem that was never wired in at all)
  — don't let a superficially identical symptom collapse two investigations
  into one ticket. File them separately with the evidence that distinguishes
  them.
