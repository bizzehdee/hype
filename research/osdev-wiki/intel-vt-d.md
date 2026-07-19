---
title: 'Intel VT-d'
source_page_id: '5063'
source_revision_id: '30803'
source_timestamp: '2026-06-20T21:39:56Z'
source_format: mediawiki
---

# Intel VT-d

> This page or section is a stub.

Intel VT-d provides an [IOMMU](IOMMU) to translate device addresses to host addresses, similar to the MMU on the CPU. It is also able to remap interrupt lines.

The location of the configuration registers can be found in the DMAR [ACPI](ACPI) table. Note, DMAR is specific to VT-d.

## DMAR
VT-d is found by locating the DMAR ACPI table, much like the [MADT](MADT).
{| class="wikitable"
|+
!Offset
!0
!1
!2  
!3  
!4
!5
!6
!7
|-
|0x0
| colspan="4" |Signature
| colspan="4" |Length
|-
|0x8
|REV
|CHK
| colspan="6" |OEMID
|-
|0x10
| colspan="8" |OEM Table ID
|-
|0x18
| colspan="4" |OEM Revision
| colspan="4" |Creator ID
|-
|0x20
| colspan="4" |Creator Revision
|HAW
|FLG
| colspan="2" |Resevred
|-
|0x28
| colspan="8" |Reserved
|-
|0x30
| colspan="8" rowspan="4" |DRHD #1
|-
|0x38
|-
|0x40
|-
|0x48
|-
|0x50
| colspan="8" rowspan="3" |DRHD #2
|-
|0x58
|-
|0x60
|-
|0x68
| colspan="8" rowspan="2" |DRHD...
|-
|0x70...
|}
Signature: "DMAR".

## Translation structures
There are two translation formats: *legacy* and *scalable*. The legacy format is simpler but lacks useful features. Importantly, it only supports second-stage page tables **which are not compatible with [Paging](X86)(X86 Paging) tables**.

This section will only discuss the scalable format, which supports first-stage translation and is directly compatible with 64-bit x86 page tables[3.6: "First-stage translation supports the same paging structures as Intel® 64 processors when operating in
64-bit mode"](^Section).

Note that the tables are all 4KiB in size and alignment. This can be used as a sanity check when defining structures.

> **Note:** Requests to the `0xFEEx_xxxx` region do not go through DMA translation tables. See the Interrupt Remapping section.

### Scalable Mode Root Table
The root table consists of 256 **Root Entries**, with each entry corresponding to a single bus. Each entry points to a single *Context Table*.

#### (Lower/Upper) Context Table
The context table is split in two equally sized halves, each half containing 128 entries for a total of 256 entries.

Each entry corresponds to a single device function.

### Scalable Mode PASID structures
The PASID tree consists of a directory and tables, similar to page tables. Each directory entry points to a table.

The [PASID](Process Address Space ID (PCIe)) is either a default value ("request-without-PASID") or included in the [TLP](Transaction Layer Packet (PCIe)) ("request-with-PASID").

### First-Stage Page Table Structures
These page tables are fully compatible with the CPU page tables. However, some of the otherwise ignored bits may be accessed and interpreted specially by the IOMMU.

#### Extended Access bit
## Interrupt Remapping
### MSI / MSI-X
Any write requests to the `0xFEEx_xxxx` region **do not go through DMA translation tables**.

## Emulation
### QEMU
To use an emulated VT-d IOMMU with support for scalable mode and first-stage translation structures, add the following argument:
```
-device intel-iommu,x-scalable-mode=on,x-flts=on
```
(`flts` is a historical acronym meaning "First-Layer Translation Structures")

#### `info mtree`
The memory regions used by the emulated devices will look like this:

<pre>
memory-region: vtd-1f.2
  0000000000000000-ffffffffffffffff (prio 0, container): vtd-1f.2
    0000000000000000-ffffffffffffffff (prio 0, i/o): vtd-1f.2-dmar
      00000000fee00000-00000000feefffff (prio 1, i/o): alias vtd-ir @vtd-ir 0000000000000000-00000000000fffff
</pre>

Note the `vtd-ir` range with higher priority than the `dmar` range.

## See also
<references />

### External Links
- [Intel® Virtualization Technology for Directed I/O (June 2022 Revision 4.0)](https://cdrdv2-public.intel.com/671081/vt-directed-io-spec.pdf)

[Virtualization](Category:X86)(Category:X86 Virtualization)
[Category:X86-64](Category:X86-64)
