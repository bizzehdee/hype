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
files injected via `debugfs -w -R "write <src> <path>"` (no `mount`, no root
needed) reproduced the same multi-extent file layout without touching the
physical drive at all. Linking `core/ext.c` + its dependencies against a
~30-line harness that calls `hype_ext_resolve()` and then walks the resolved
extent map to reconstruct the full file, comparing against the original bytes,
conclusively ruled OUT the extent resolver and the runtime extent-to-sector
translation as the cause (both round-tripped byte-identical) — narrowing the
real defect to `boot/main.c`'s device/partition-scanning orchestration, all
without a second boot.

The NTFS case needed no such harness at all: reading `fw_1_resolve_on_any_fs()`
directly showed it only ever calls the FAT32/exFAT/ext resolvers — NTFS is not
attempted, period. The error message even names the three filesystems it
tried. One `grep`/read settled it; no reproduction needed once the dispatcher
itself was read instead of assumed.

## The lesson

- Before spending a physical boot to narrow down a storage/filesystem defect,
  ask whether the suspected component is plain host-buildable C (it usually
  is — `core/`'s FS resolvers, `blk_image.c`, `blk_backend.c` all avoid
  privileged instructions by design). If so, reproduce it in a throwaway
  loopback image first.
- `debugfs -w` can inject files into an ext2/3/4 image without `mount` or
  root — useful when the sandbox/session has no `sudo`. Watch its output for
  `Could not allocate block`: it means the loopback image is too small for the
  file, not a real defect.
- A round-trip test (resolve → reconstruct via the extent map → byte-compare
  against the source) is stronger evidence than "it resolved without an
  error" — it catches boundary-crossing bugs a bare resolve-success check
  would miss.
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
