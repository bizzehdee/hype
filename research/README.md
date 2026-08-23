# research/ — archived hardware & specification sources

This directory is the local archive of external primary sources —
vendor developer manuals (AMD APM, Intel SDM), datasheets, and any other
hardware/spec documents consulted while building this hypervisor. The
rule is in the `research-provenance` skill (`.claude/skills/research-provenance/`),
summarised in `AGENTS.md`; the short version:

**Check order before any web search or download:** (1) the relevant board
ticket's description/comments, then (2) this directory, then — only if
neither has it — (3) the web. The ticket description/comments are the
first stop; this directory holds the full documents behind them.

**When a manual/datasheet is fetched:** drop the PDF (or exact source
document) here with a descriptive, versioned filename, add a row to the
table below (what it is, revision, origin URL), and write the specific
facts used — section/table numbers, field offsets, bit meanings, exact
values — into the ticket it was for, pointing back at the file.

In-tree primary sources (the vendored `edk2/` tree, QEMU headers) are
authoritative for their own formats and are cited by repo path instead;
this archive is only for external documents not already in the repo.

## Copyright

The manuals archived here are **copyright of their respective owners**
(AMD, Intel) and are redistributed by them for developer reference. They
are kept in this directory only as an offline engineering reference for
building this project; they are not part of the project's own GPLv3
source and their copyright/licensing is unchanged by inclusion here. Do
not treat them as project-licensed material.

## Archived documents

| File | Document | Revision | Source |
|------|----------|----------|--------|
| `24593_3.44_APM_Vol2.pdf` | AMD64 Architecture Programmer's Manual, Vol. 2 — System Programming (SVM/VMCB) | pub. 24593, Rev. 3.44 | https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2 |
| `325462-092-sdm-vol-1-2abcd-3abcd-4.pdf` | Intel® 64 and IA-32 Architectures Software Developer's Manuals — combined volume set (Vol. 1, 2ABCD, 3ABCD, 4) | order 325462, rev. 092 | https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html |
| `t10-03-388r2-spc3-sbc2-nonvolatile-caches.pdf` | T10 SPC-3/SBC-2 Nonvolatile Caches proposal | 03-388r2, 10 March 2004 | https://www.t10.org/ftp/t10/document.03/03-388r2.pdf |
| `microsoft-fat32-specification-v1.03.pdf` | Microsoft EFI FAT32 File System Specification | Version 1.03, 6 December 2000 | https://www.win.tue.nl/~aeb/linux/fs/fat/fatgen103.pdf (archived mirror of the Microsoft specification) |
| `microsoft-exfat-specification-2026-08-11.html` | Microsoft exFAT File System Specification | Microsoft Learn snapshot, 11 August 2026 | https://learn.microsoft.com/en-us/windows/win32/fileio/exfat-specification |
| `linux-ext4-blockmap-2026-08-11.html` | Linux kernel ext4 block maps and extent trees | kernel documentation snapshot, 11 August 2026 | https://www.kernel.org/doc/html/latest/filesystems/ext4/blockmap.html |
| `linux-ext4-bitmaps-2026-08-11.html` | Linux kernel ext4 block and inode bitmaps | kernel documentation snapshot, 11 August 2026 | https://www.kernel.org/doc/html/latest/filesystems/ext4/bitmaps.html |
| `linux-ext4-group-descriptors-2026-08-11.html` | Linux kernel ext4 block group descriptors | kernel documentation snapshot, 11 August 2026 | https://www.kernel.org/doc/html/latest/filesystems/ext4/group_descr.html |
| `linux-ext4-inodes-2026-08-11.html` | Linux kernel ext4 inode structure | kernel documentation snapshot, 11 August 2026 | https://www.kernel.org/doc/html/latest/filesystems/ext4/inodes.html |
| `linux-ext4-journal-2026-08-11.html` | Linux kernel ext4 jbd2 journal format | kernel documentation snapshot, 11 August 2026 | https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html |
| `linux-ext4-directory-2026-08-22.html` | Linux kernel ext4 directory entry format, checksum tail, htree/dx_root layout | kernel documentation snapshot, 22 August 2026 | https://www.kernel.org/doc/html/latest/filesystems/ext4/directory.html |
| `microsoft-hyper-v-tlfs-hypercall-interface-94373af.md` | Microsoft Hyper-V TLFS Hypercall Interface | commit `94373af`, 15 December 2025 | https://github.com/MicrosoftDocs/Virtualization-Documentation/blob/94373af503f83b800ac002911f5d137a53392656/virtualization/hyper-v-on-windows/tlfs/hypercall-interface.md |
| `ntfs-doc-russon-fledel.pdf` | "NTFS Documentation" (Richard Russon, Yuval Fledel) -- the linux-ntfs project's format reference | fetched 22 Aug 2026 | https://dubeyko.com/development/FileSystems/NTFS/ntfsdoc.pdf (mirror; this particular export is a thin/truncated 19-page snapshot -- the #417/#337 work instead relies on the empirical-validation-against-real-volumes method documented below, per this file's own "NTFS (#337)" and "NTFS $Bitmap cluster allocation (#417)" entries) |

## Archived wiki exports

| Source | Snapshot | Derived Markdown | Notes |
|------|----------|----------|------|
| `OSDev+Wiki-20260719190820.xml` | 2026-07-19 19:08:20 | `osdev-wiki/` | One Markdown file per non-template article; exported templates are expanded into their use sites. |

### Key extracts captured against tasks

- **AMD APM Vol 2 (`24593_3.44_APM_Vol2.pdf`).** SVM/VMCB work — §15 (SVM:
  VMRUN, #VMEXIT, EVENTINJ/VINTR §15.20/§15.21, intercepted-#PF semantics
  §15.12.15, decode assists, MSRPM/IOPM layout §15.11) and Appendix B
  (VMCB layout / state-save-area field offsets). §7.6.5 identifies
  Fn8000_0008 and Fn8000_001E as processor-topology sources; Fn8000_001E
  returns the extended APIC ID in EAX and the compute-unit description in
  EBX. Cited throughout the M2 (SVM), FW-1, CPUMSR, M4-6b, and #378 task
  notes.
- **Intel SDM (`325462-092-sdm-vol-1-2abcd-3abcd-4.pdf`).** The Intel-host
  counterpart reference (VMX/VT-x, IA-32 system programming) for the
  mandatory Intel real-hardware validation pass (AGENTS.md testing gate);
  cite the specific volume/§ against the task when used.
- **T10 nonvolatile-cache proposal (`t10-03-388r2-spc3-sbc2-nonvolatile-caches.pdf`).** #377
  uses §5.20 and table 3. SYNCHRONIZE CACHE(10) has opcode 35h. A zero LBA and
  zero block count select all remaining logical blocks. SYNC_NV=0 requires
  synchronization to the medium, and IMMED=0 withholds status until completion.
- **FAT32 specification (`microsoft-fat32-specification-v1.03.pdf`).** Sparse-writer
  planning uses the FAT entry rules and directory-entry definition. A zero-length
  file has first cluster zero (page 21). `DIR_FileSize` is the file size in bytes
  (page 24), while allocation is expressed only by the singly linked FAT cluster
  chain. FAT32 has no field that can identify a logical hole inside that chain.
- **exFAT specification (`microsoft-exfat-specification-2026-08-11.html`).** Sparse-writer
  planning uses §§4.1, 6.3.5-6.3.6, and 7.6.5-7.6.7. `DataLength` describes the
  allocated stream. `ValidDataLength` describes the contiguous prefix written by
  the application. Implementations must return zeroes beyond `ValidDataLength`.
  The allocation remains a contiguous run or FAT chain; the format has no logical
  index for an arbitrary unallocated hole.
- **Linux ext4 documentation (`linux-ext4-*-2026-08-11.html`).** Sparse-writer
  planning uses `blockmap` for logical extent indices and unwritten extents,
  `bitmaps` and `group-descriptors` for block allocation and free counts,
  `inodes` for mappings, sizes and inode checksums, and `journal` for jbd2
  metadata transactions. Allocation changes metadata and therefore cannot use
  #204's journal-bypass reasoning, which applies only to in-place data writes.
- **Linux ext4 directory documentation (`linux-ext4-directory-2026-08-22.html`).**
  #498 (namespace mutation) uses `struct ext4_dir_entry_2` (inode/rec_len/
  name_len/file_type/name, 8-byte header) and the `struct ext4_dir_entry_tail`
  checksum fake-entry every leaf directory block carries under
  RO_COMPAT_METADATA_CSUM: 12 bytes, `det_reserved_zero1`(inode)=0,
  `det_rec_len`=12, `det_reserved_zero2`(name_len)=0, `det_reserved_ft`
  (file_type)=0xDE, `det_checksum` = crc32c seeded with the SAME i_csum_seed
  #495 already computes per-inode (fs seed chained with the directory's own
  inode number + generation), hashed over the block up to but excluding the
  tail. Confirms `EXT4_INDEX_FL` = 0x1000 (already used by core/tests/test_ext.c's
  htree fixture) and that an htree directory's root block starts with real
  '.'/'..' entries followed by a `dx_root_info` header masquerading as more
  directory entries -- exactly why a linear insertion into an htree directory
  corrupts the index instead of merely being suboptimal.
- **Hyper-V TLFS (`microsoft-hyper-v-tlfs-hypercall-interface-94373af.md`).** #300
  uses "Hypercall Inputs", "Hypercall Outputs", "Hypercall Status Codes", and
  "Establishing the Hypercall Interface (x86/x64)". The call code is input bits
  15:0 in RCX for x64. The 64-bit result is returned in RAX, with the status in
  bits 15:0. `HV_STATUS_INVALID_HYPERCALL_CODE` identifies an unknown call.
  `HV_X64_MSR_HYPERCALL` bit 0 enables a page whose GPFN is bits 63:12. A
  nonzero Guest OS ID is required before enablement. The page must be fully
  within the guest GPA space. A call enters at the page start and the page must
  provide near-return behavior.

### #440 ICH9 AHCI primary-source provenance

- **QEMU ICH9 AHCI model (upstream source, consulted 14 August 2026).** #440 uses
  `https://gitlab.com/qemu-project/qemu/-/raw/master/hw/ide/ich.c` (LGPL-2.1-or-later;
  consulted, not copied). `pci_ich9_ahci_realize()` defines the compatible Q35
  contract: `8086:2922`, revision `02`, cache line `08`, AHCI mode at config
  `0x90` bit 6, I/O BAR0--4 sizes 8/4/8/4/32, a 2 KiB memory BAR5, six ports,
  and a capability chain with 64-bit MSI at `0x80` followed by SATA at `0xA8`.
  The vendored EDK2 `OvmfPkg/Library/QemuBootOrderLib/QemuBootOrderLib.c:809-845`
  independently confirms the Q35 `Pci(0x1F,0x2)` placement.

## Online reference links (external, not archived)

Code/spec links gathered for reference. Not downloaded into this tree (code
repos under their own licenses; the Intel PDFs are superseded by the archived
combined SDM above). Ratings are relative to hype's actual surfaces (AMD SVM
host; guest device models + MMIO decode; the Intel-VMX path is future work).

| Link | What it is | Usefulness to hype |
|------|------------|--------------------|
| http://www.intel.com/Assets/PDF/manual/253669.pdf | Intel SDM Vol. 3B (system programming, incl. APIC) | ★★ now — the **APIC/LAPIC-timer/IPI** chapter backs M8-0b inc 5 (AP LAPIC timer) and the `sysvec_call_function` spin lead. Superseded by the archived combined SDM (325462); use that copy. |
| http://www.intel.com/Assets/PDF/manual/253667.pdf | Intel SDM Vol. 2B (instruction set reference) | ★★ — instruction encoding, cross-checks `mmio_decode.c`. Also in the archived combined SDM. |
| http://lxr.free-electrons.com/source/arch/x86/kvm/vmx.c | KVM VMX implementation (GPLv2) | ★ future — reference for the Intel-VMX ops path (currently a stub). NOT for SVM (AMD APM is the SVM authority). License: GPLv2 — read for understanding, don't copy into GPLv3-with-care. |
| http://bochs.cvs.sourceforge.net/viewvc/bochs/bochs/cpu/vmx.cc | Bochs VMX emulator (LGPLv2) | ★ future — clean, readable VMX behavior reference for the Intel path. |
| https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/vmx_pio.c | ZeldaOS PIO exit sub-handler | ★★ — small VMM's port-I/O dispatch; cross-check for hype's IOIO handling + the **spin investigation** (guest polling a mis-modeled port). |
| https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/vmx_instruction_decoding.c | ZeldaOS MMIO mov decode | ★★ — direct comparison for `arch/x86_64/cpu/mmio_decode.c`. |
| https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/device_8259pic.c | ZeldaOS 8259 PIC model | ★★ — cross-check `devices/pic.c` (esp. spurious-IRQ / ISR-read behavior). |
| https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/device_8253pit.c | ZeldaOS 8253 PIT model | ★★★ — directly relevant to the **spin/timer investigation**: compare channel counting + calibration behavior against `devices/pit.c`. |
| https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/device_keyboard.c | ZeldaOS 8042 keyboard model | ★ — cross-check `devices/ps2_keyboard.c`. |
| https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/device_serial.c | ZeldaOS 16550 serial model | ★ — cross-check `devices/guest_uart.c`. |
| https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/device_video.c | ZeldaOS 16-color video (MMIO) | ✩ low — hype uses GOP/ramfb, not legacy 16-color text MMIO. |

Note on licenses: the KVM (GPLv2) and Bochs (LGPLv2) sources are for
*understanding*, not copy-paste — hype is GPLv3 and its device/decode logic is
written fresh from the primary specs. ZeldaOS (check its repo license) is a
useful "how another small VMM structured this" comparison, same rule.

## NTFS (#337)

No external specification document was archived for the NTFS resolver: it was
written from the ntfs-3g layout headers' publicly documented structures (boot
sector, FILE record + update sequence arrays, attribute headers, mapping
pairs/runlists, $ATTRIBUTE_LIST, $INDEX_ROOT/$INDEX_ALLOCATION/$BITMAP,
$UpCase, $VOLUME_INFORMATION) and then **empirically validated field-by-field
against genuine volumes**: mkntfs-created images populated through the kernel
ntfs-3g driver, cross-checked with `ntfsinfo` dumps, byte-exact reads
(including sparse runs and a fragmented multi-extent file), and a clean
`ntfsfix -n` after hype's in-place writes. Two behaviours worth recording
because they are easy to get wrong and were caught by that validation, not by
documentation:

- **Each attribute extent's mapping pairs are self-contained**: the first
  delta of every extent is relative to LCN 0, not to the previous extent's
  last LCN (ntfs-3g decompresses each extent from zero and merges by VCN).
- **$VOLUME_INFORMATION's flags** live at value offset 10 (8 reserved bytes,
  then major/minor version bytes), not offset 8.

## NTFS $Bitmap cluster allocation (#417)

`$Bitmap` (MFT record 6, unnamed non-resident `$DATA`) is one bit per cluster,
**LSB-first within each byte**: bit 0 of byte 0 is cluster 0. 1 == allocated.
No separate free-cluster counter exists anywhere on disk (unlike ext2's
superblock `s_free_blocks_count`) -- a real driver and chkdsk always derive it
by popcounting the bitmap, so hype's allocator never has a redundant counter
to keep in sync, only the bitmap bytes themselves. `hype_ntfs_cluster_alloc()`
first-fits (NTFS has no on-disk-visible allocation-policy requirement; a
proximity/zone heuristic like ntfs-3g's own allocator uses is a placement
choice, not a format rule, so first-fit is a complete, correct implementation,
not a simplification pending later work).

**Empirically validated against a genuine `mkntfs`-created, `ntfs-3g`-written
volume** (built and inspected on a real Ubuntu box over SSH, since this dev
sandbox has no root/mount access): `ntfscluster -c 0` independently confirmed
cluster 0 belongs to `$Boot` before any hype write touched the volume, so
hype's scanner correctly skips real system-file occupancy rather than
starting from an assumed-clean bitmap. Mounting the same image with
`-o show_sys_files` exposes `$Bitmap` as a normal readable file
(`/$Bitmap`) -- `xxd`-ing the exact byte covering the allocated/freed run
showed the raw bitmap byte flip from `7f00` (bits 16-22 set, 23-31 clear) to
`ffff` (allocate 10 clusters from LCN 23: bits 23-32 also set) and back to
`7f00` after `hype_ntfs_cluster_free()`, byte-exact, with `ntfsfix -n`
reporting the volume clean at every step. This is stronger evidence than
`ntfsfix` alone: `ntfsfix -n` checks volume-level consistency (dirty flag,
`$MFT`/`$MFTMirr` agreement, boot sector), not bitmap bit-for-bit correctness,
so the raw `$Bitmap` byte dump is what actually proves the LSB-first bit
convention was implemented correctly, independent of hype's own code.

## NTFS non-resident $DATA's Highest VCN field (#418)

A non-resident attribute header has a field hype's OWN read path never
needed and so never modeled: **Highest VCN at value offset +0x18** (8 bytes,
between Starting VCN at +0x10 and the mapping-pairs offset at +0x20 --
`ntfs_attr_t` in `core/ntfs.c` only tracks `start_vcn` and `rl_off`, skipping
straight over it). `runlist_decode()` derives a file's coverage entirely from
walking the mapping pairs themselves, so a stale Highest VCN is invisible to
every one of hype's OWN read-side tests -- host unit tests and hype's own
`hype_ntfs_resolve()` both parsed an appended runlist with a stale Highest
VCN perfectly correctly.

**Empirically discovered the hard way**: the first version of
`hype_ntfs_data_append()` (appends a new run) updated the runlist bytes and
the allocated/real/initialized size fields but never touched Highest VCN.
Every unit test passed. Appending to a genuine `mkntfs`-created,
`ntfs-3g`-written file on the Intel validation box, though, made `ntfs-3g`
refuse to read the file with `EIO` -- **from VCN 0**, i.e. it refused even
the ORIGINAL, completely untouched bytes, not just the newly appended
region. Isolated by elimination (each ruled out with its own real-volume
test): not `$Bitmap` (a `noop` record rewrite round-tripped byte-identical;
a pure allocated-size-only field bump also read back fine); not the runlist
encoding itself (manually decoded the on-disk mapping-pairs bytes after the
append and confirmed every run, delta, and terminator were spec-correct,
byte-for-byte); not $MFTMirr (this volume's mirror covers only records 0-3,
record 64 is untouched by it). The only field left unaccounted was Highest
VCN, still holding the OLD run count after a new run was appended -- a
real driver validates it against the runlist's actual VCN coverage and, on
mismatch, refuses the whole attribute rather than serving what it can.

Fix: `hype_ntfs_data_append()` now advances Highest VCN by the same
`cluster_count` it appends. Re-verified against the same real volume: the
file (and its unmodified original bytes, exact byte-for-byte) reads back
successfully after the append, both when only allocated size grows
(pure preallocation, real/initialized size unchanged -- valid, spec-legal
NTFS) and when real/initialized size grow to match (a full non-sparse
append). **Any future non-resident-attribute writer that adds or removes
runs must update Highest VCN too** -- it is easy to miss because nothing
in hype's own read path depends on it, so only a genuine external driver
catches its absence.

## NTFS AllocatedSize does not always exclude sparse holes on-disk (#419)

The spec model (and this ticket's own initial assumption) is that a sparse
non-resident attribute's on-disk **AllocatedSize** field excludes sparse
(`HOLE`) runs -- only bytes truly backed by clusters count. **Measured
counter-example**: a genuinely sparse file created via `ntfs-3g` itself
(`truncate` past written data, then `dd seek=` to write past the gap) had
AllocatedSize on disk ALREADY equal to the file's full logical size (all 5
clusters' worth) even though only 3 were actually backed -- `ntfsinfo`
separately reported the true backed-byte count under a **different** label,
"Compressed size", derived by walking the runlist, not read from the
AllocatedSize field. So on at least this `ntfs-3g` version/code path, the
on-disk field itself does not reliably track true backing.

Consequence for `hype_ntfs_hole_fill()`: computing the new AllocatedSize as
"old on-disk AllocatedSize + newly-filled bytes" silently double-counts
on a file with this looseness (28672 observed instead of the correct
20480 in the concrete case above). Fixed by **recomputing AllocatedSize
from scratch** every time -- sum every DATA run's cluster count across the
WHOLE attribute (prefix runs walked to find the target hole, the fill
itself, and the tail runs after it) and multiply by cluster size, never
trusting the old field's arithmetic relationship to the new one. This is
correct regardless of which convention produced the file. Re-verified on
the same real volume: AllocatedSize came out exactly right (20480) after
this fix, content read back byte-for-byte unchanged, and the `SPARSE_FILE`
attribute flag correctly cleared once no `HOLE` runs remained.

**Lesson for #420+**: don't trust an existing size/count field's absolute
value as a baseline to increment -- when a writer can be handed a file
built by an external, less-rigorous path, recomputing derived fields from
the authoritative source (here, the runlist itself) is safer than trusting
prior bookkeeping to have been consistent.

## NTFS $MFT record allocation: pre-formatted-but-unused records (#420)

`mkntfs` pre-initializes a batch of `$MFT` records with a valid `FILE`
magic, fixups, and a real (nonzero) sequence number, all still marked NOT
in use -- confirmed empirically: allocating an apparently-fresh record on a
real `mkntfs` volume came back with sequence number 17, not 1, because the
raw bytes at that slot already held a valid record with sequence number 16
stored (`hype_ntfs_mft_record_alloc()` reads the raw bytes first and reuses
+ bumps whatever sequence number is already there, rather than assuming an
all-zero, never-touched slot). Don't assume a bit being clear in `$MFT`'s
own `$BITMAP` means the corresponding record is all-zero bytes -- it is
just as likely to be a real, structurally valid, previously-prepared or
previously-freed record.

A record built by `hype_ntfs_mft_record_alloc()` alone (magic, fixups, an
empty attribute list) is intentionally not yet a "file" any higher-level
tool recognizes -- `ntfsinfo` correctly reports "No STANDARD_INFORMATION in
base record N" for it, which is the EXPECTED result of this slice's scope
boundary (adding `$STANDARD_INFORMATION`/`$FILE_NAME`/a directory entry is
#423/#425's job), not a defect. What matters at this layer is that the
record's own bytes are spec-valid (fixups round-trip, sequence number and
flags correct) and that `ntfsfix -n` stays clean through alloc, free, and
immediate re-allocation of the same slot (verified: sequence number
correctly advances 16 -> 17 (alloc) -> 18 (free) -> 19 (realloc) across
the cycle) -- i.e. the record-slot bookkeeping is sound even though the
record has no visible file content yet.

## NTFS $I30 index insert: named resident attributes, and UTF-16 keys (#421)

Two real bugs found only by testing against a genuine `mkntfs`/`ntfs-3g`
directory (both invisible to host unit tests built on hand-crafted
fixtures, for related reasons):

1. **`$INDEX_ROOT`'s resident value offset is not a fixed 0x18.** Any
   "indexed" resident attribute -- in practice always true for
   `$INDEX_ROOT`/`$INDEX_ALLOCATION`/`$BITMAP` -- carries an attribute
   NAME (`$I30`, 4 UTF-16 units = 8 bytes) placed between the standard
   resident header and the value, so `a.val_off` on a real directory came
   back `0x20`, not `0x18`. hype's own `attr_parse()` already reads
   `val_off` dynamically and was never wrong; the bug was in the
   NEW write-side code computing the attribute's total on-disk length
   from a hardcoded `0x18 + value_length` instead of `a.val_off +
   value_length`. Every synthetic host-test fixture uses an UNNAMED
   $INDEX_ROOT (val_off genuinely 0x18), so this never showed up until a
   real volume was tried. **Fixed: always derive the header size from the
   attribute's own val_off, never assume a constant** -- the same lesson
   #418's mapping-pairs code already had to learn for a different field.
2. **The insertion key must be UTF-16LE, not byte-packed ASCII.** The new
   entry's comparison key (`new_key`) was filled one BYTE per character
   (`new_key[i] = name[i]`), but the collation comparator reads UTF-16
   code units (2 bytes each, matching every on-disk key and
   `index_build_entry()`'s own encoding) -- so every collation compare
   read garbage and every insert landed in an essentially-random position
   in the sorted array. **This passed every host unit test**: hype's own
   `hype_ntfs_resolve()`/`dir_lookup()` scan entries LINEARLY and never
   cared about sort order, so a wrongly-placed entry still resolved fine
   through hype's own code. Only a REAL `ntfs-3g` mount exposed it: `ls`
   (a linear `readdir()`) listed the new file, but `stat`/`cat` (`ntfs-3g`'s
   own by-name lookup, which apparently does depend on collation order)
   reported "No such file or directory" for the exact same name `ls` had
   just shown. Fixed by widening the key to UTF-16LE before comparing.
   **Lesson for #422+**: a host-test assertion that only checks
   "resolve() finds it" is not sufficient for anything touching on-disk
   ORDER (index sort order, runlist VCN sequencing, attribute list VCN
   ranges) -- add a test that reads the raw bytes back and checks the
   actual on-disk arrangement, and treat a real-driver check as the
   decisive one, not a formality, whenever ordering is part of the
   contract.

Both fixes re-verified against the same real `mkntfs`/`ntfs-3g` directory:
inserting a second name for an existing file made it openable and
readable under the new name (correct inode), and deleting the original
name left the alias intact and fully accessible -- both via a REAL
`ntfs-3g` mount, not just hype's own resolver.

## Test helper trap: reading raw record bytes without fixup_apply() (#423)

Not a hype bug, but a test-harness one worth recording so it isn't
re-learned the hard way: `core/tests/test_ntfs.c` had a helper
(`entry_offset_by_name`, added for #421) that peeked at a record's bytes
directly (`rec_ptr(n)`, a raw pointer into the fixture's backing buffer)
instead of going through `hype_ntfs_record_read()`. For most positions
this is harmless, but the on-disk bytes at every sector's last 2 bytes
(offset 510-511 and 1022-1023 of a 1024-byte, 2-sector record) are USA
fixup territory: `hype_ntfs_record_write()` legitimately overwrites them
with the USN, saving the real bytes into the update sequence array, and
only `fixup_apply()` (called by `hype_ntfs_record_read()`) restores them.
A new #423 test happened to insert a directory entry whose last 2 bytes
landed exactly on such a boundary, and the raw-peek helper read the
stamped USN there instead of the real bytes -- looking exactly like data
corruption. It wasn't: `hype_ntfs_index_insert()`'s own in-memory
splice was byte-perfect (confirmed by temporarily dumping it before the
fixup-stamping write), and the production read paths
(`hype_ntfs_unlink()`'s internal `dir_lookup()`, which does go through
`record_read()`) found the entry correctly the whole time. Fixed by
making the test helper call `hype_ntfs_record_read()` too. **Any test
helper that inspects a record's raw bytes directly, instead of through
the read API, will occasionally "see" fixup corruption that was never
really there** -- always read through `hype_ntfs_record_read()`.

## NTFS $LogFile / USN journal (#416)

#416 (plan.md §10 decision 64) descoped `$LogFile` replay entirely: Microsoft's LFS (Log File
Service) format is undocumented outside their own driver source, and ntfs-3g -- this project's
own reference -- does not implement replay either, it refuses a dirty volume exactly like hype's
existing `#337` mount check already does. No document was archived for `$LogFile` because none
was used: the decision is to keep refusing, not to parse it.

The USN change journal is different: `USN_RECORD_V2` is a **public, stable Win32 API structure**
(`winioctl.h`, used by `FSCTL_READ_USN_JOURNAL`/`FSCTL_ENUM_USN_DATA` and documented on Microsoft
Learn), not an internal format, so it has genuine ground truth. Field layout used by
`core/ntfs_journal.c`, all `little-endian`, all well-known/stable since Windows 2000 and unchanged
through USN_RECORD_V2 (V3/V4 add 128-bit file IDs for ReFS and are out of scope -- NTFS always
uses V2's 64-bit `MFT_SEGMENT_REFERENCE` file IDs):

| Offset | Size | Field |
|---|---|---|
| 0x00 | 4 | RecordLength (total record size, DWORD-aligned) |
| 0x04 | 2 | MajorVersion (2) |
| 0x06 | 2 | MinorVersion (0) |
| 0x08 | 8 | FileReferenceNumber (MFT record# in low 48 bits + sequence# in high 16) |
| 0x10 | 8 | ParentFileReferenceNumber (same shape, for the containing directory) |
| 0x18 | 8 | Usn (this record's own journal-relative byte offset) |
| 0x20 | 8 | TimeStamp (FILETIME) |
| 0x28 | 4 | Reason (USN_REASON_* bitmask -- FILE_CREATE 0x100, DATA_EXTEND 0x2, RENAME_NEW_NAME 0x2000, FILE_DELETE 0x200, etc.) |
| 0x2C | 4 | SourceInfo |
| 0x30 | 4 | SecurityId |
| 0x34 | 4 | FileAttributes |
| 0x38 | 2 | FileNameLength (bytes, UTF-16) |
| 0x3A | 2 | FileNameOffset (from record start; 0x3C for V2) |
| 0x3C | var | FileName (UTF-16LE, no NUL terminator) |

`$Extend\$UsnJrnl`'s `$J` (unnamed on some volumes, `$J` alternate stream in the common case)
data stream is a sparse, ever-growing sequence of these records; `$Max` holds the journal's
configured MaximumSize/AllocationDelta. hype only ever APPENDS a record when it is already
present and active (never creates/enables a journal itself) -- matching #416's scope of
maintaining an existing journal, not establishing one.
