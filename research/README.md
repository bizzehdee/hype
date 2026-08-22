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
