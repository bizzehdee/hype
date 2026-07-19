---
title: 'AMD-Vi IOMMU'
source_page_id: '4987'
source_revision_id: '30805'
source_timestamp: '2026-06-20T22:26:35Z'
source_format: mediawiki
---

# AMD-Vi IOMMU

> This page or section is a stub.

AMD-Vi provides a [IOMMU](IOMMU) to translate "Device Virtual Addresses" (DVAs) to "System Physical Addresses" (SPAs). Without nested paging, it is equivalent to translating linear/virtual addresses to physical addresses. It is also able to remap interrupt lines.

The IOMMU can be located in the PCI configuration space and is identified by checking class=0x8, subclass=0x6 and progif=0x0. None of the BAR registers are used, instead registers in capability space (id=0xf) must be used.

## Overview
The IOMMU requires several in-memory structures to operate.
Communication with the IOMMU is primarily done through ring buffers to improve throughput.

### Device table
The device table describes how to handle transactions for devices behind the IOMMU.
There is one entry for each device, up to 65536 (the maximum for a single PCI segment).

By default, the array must be contiguous in memory. As each entry is 32 bytes it means a maximum of 2MiB is required, though it can be shorter if the start/end range contains no devices. The IOMMU may support a segmented device table to save further memory.

### I/O page tables
The I/O page tables are compatible with [long-mode](Paging#64-Bit_Paging) page tables. It uses a number of the AVL bits to set device permissions and other attributes.

### Interrupt remapping
### Nested translation
### Command buffer
### Event log
## Device table
{| class="wikitable"
|+ Device Table Entry (DTE)
!
! 31
! 30
! 29
! 28
! 27
! 26
! 25
! 24
! 23
! 22
! 21
! 20
! 19
! 18
! 17
! 16
! 15
! 14
! 13
! 12
! 11
! 10
!  9
!  8
!  7
!  6
!  5
!  4
!  3
!  2
!  1
!  0
|-
! +0
|colspan="20"| Host Page Table Root Pointer[31:12]
|colspan="3" | Mode
|colspan="2" | HAD
|colspan="5" | *res*
|colspan="1" | TV
|colspan="1" | V
|-
! +4
|colspan="1" | *res*
|colspan="1" | IW
|colspan="1" | IR
|colspan="3" | GCR3 TRP[14:12]
|colspan="2" style="writing-mode:vertical-rl"| GLX
|colspan="1" | GV
|colspan="1" style="writing-mode:vertical-rl"| GIoV
|colspan="1" style="writing-mode:vertical-rl"| GPRP
|colspan="1" | PPR
|colspan="20"| Host Page Table Root Pointer[51:32]
|-
! +8
|colspan="16"| GCR3 Table Root Pointer[30:15]
|colspan="16"| Domain ID[15:0]
|-
! +12
|colspan="21"| GCR3 Table Root Pointer[51:31]
|colspan="1" style="writing-mode:vertical-rl"| SATS
|colspan="2" style="writing-mode:vertical-rl"| SysMgt
|colspan="1" | EX
|colspan="1" | SD
|colspan="1" style="writing-mode:vertical-rl"| Cache
|colspan="2" style="writing-mode:vertical-rl"| IoCtl
|colspan="1" | SA
|colspan="1" | SE
|colspan="1" | I
|-
! +16
|colspan="26"| Interrupt Table Root Pointer[31:6]
|colspan="1" | IG
|colspan="4" | IntTabLen
|colspan="1" | IV
|-
! +20
|colspan="1" style="writing-mode:vertical-rl"| Lint1Pass
|colspan="1" style="writing-mode:vertical-rl"| Lint0Pass
|colspan="2" style="writing-mode:vertical-rl"| IntCtl
|colspan="1" style="writing-mode:vertical-rl"| HPTMode
|colspan="1" style="writing-mode:vertical-rl"| NMIPass
|colspan="1" style="writing-mode:vertical-rl"| EIntPass
|colspan="1" style="writing-mode:vertical-rl"| InitPass
|colspan="2" style="writing-mode:vertical-rl"| GuestPagingMode
|colspan="2" | *res*
|colspan="20"| Interrupt Table Root Pointer[51:32]
|-
! +24
|colspan="16"| GDeviceID[15:0]
|colspan="1" style="writing-mode:vertical-rl"| vImuEn
|colspan="15"| *reserved*
|-
! +28
|colspan="8" | SnoopAttribute
|colspan="1" style="writing-mode:vertical-rl"| Mode0FC
|colspan="1" style="writing-mode:vertical-rl"| AttrV
|colspan="6" | *reserved*
|colspan="16"| GuestID[15:0]
|}

## I/O page tables
{| class="wikitable"
|+ Page Table Entry (PTE)
! 63
! 62
! 61
! 60
! 59
! 58:52
! 51:12
! 11:9
! 8:7
! 6
! 5
! 4:1
! 0
|-
| *ign*
| IW
| IR
| FC
| U
| *resv*
| physical address
| next level
| *ign*
| D
| A
| *ign*
| P
|}

{| class="wikitable"
|+ Page Directory Entry (PDE)
! 63
! 62
! 61
! 60:52
! 51:12
! 11:9
! 8:6
! 5
! 4:1
! 0
|-
| *ign*
| IW
| IR
| *resv*
| physical address
| next level
| *ign*
| A
| *ign*
| P
|}

## Commands
Each command entry is 128 bits in size.

{| class="wikitable"
|+ Command entry
|-
!colspan="1"  style="width:4em" | Offset
!colspan="1"  style="width:4em" | 63:60
!colspan="15" style="width:60em"| 59:0
|-
| +0
| Opcode
| First opcode dependent operand
|-
| +8
|colspan="2"| Second opcode dependent operand
|}

To send commands, the **Command Buffer Base Register** must be programmed. It must point to a ring buffer aligned to a 4KiB boundary.
The **Command Buffer Head Pointer Register** and **Command Buffer Tail Pointer Register** are automatically reset to 0 when the base register is written to.

{| class="wikitable"
|+ Command Buffer Base Register
!colspan="1"  style="width:4em" | 63:60
!colspan="1"  style="width:4em" | 59:56
!colspan="1"  style="width:4em" | 55:52
!colspan="10" style="width:40em"| 51:12
!colspan="3"  style="width:12em"| 11:00
|-
| /
| Length
| /
|colspan="10"| Base
| /
|-
|}

{| class="wikitable
|+ Length encoding
! Encoding
! Number of entries
! Byte size
|-
| 0-7
|colspan="2"| (reserved)
|-
| 8
| 256 entries
| 4 KiB
|-
| 9
| 512 entries
| 8 KiB
|-
|colspan="3" style="text-align:center"| ...
|-
| 15
| 32768 entries
| 512 KiB
|}

{| class="wikitable"
|+ Command Buffer Head Pointer Register
!colspan="11" style="width:44em"| 63:19
!colspan="4"  style="width:16em"| 18:4
!colspan="1"  style="width:4em" | 3:0
|-
|colspan="11"| /
|colspan="4" | Head pointer
|colspan="1" | /
|}

{| class="wikitable"
|+ Command Buffer Tail Pointer Register
!colspan="11" style="width:44em"| 63:19
!colspan="4"  style="width:16em"| 18:4
!colspan="1"  style="width:4em" | 3:0
|-
|colspan="11"| /
|colspan="4" | Tail pointer
|colspan="1" | /
|}

To notify the IOMMU, the Tail Pointer Register must be incremented, wrapping if necessary (modulo ring buffer size).

The IOMMU will increment the Head Pointer Register when a command has been fetched.
This does not mean the command has been processed! To ensure a command has been processed, use COMPLETION_WAIT.

Commands are only processed when **CmdBufRun=1**. Modifying the Base Register or the Head Pointer Register is only allowed when CmdBufRun=0.

### 1. COMPLETION_WAIT
### 2. INVALIDATE_DEVTAB_ENTRY
### 3. INVALIDATE_IOMMU_PAGES
### 4. INVALIDATE_IOTLB_PAGES
### 5. INVALIDATE_INTERRUPT_TABLE
### 6. PREFETCH_IOMMU_PAGES
### 7. COMPLETE_PPR_REQUEST
### 8. INVALIDATE_IOMMU_ALL
### 9. INSERT_GUEST_EVENT
### 10. RESET_VMMIO
## Events
### 1. ILLEGAL_DEV_TABLE_ENTRY
### 2. IO_PAGE_FAULT
### 3. DEV_TAB_HARDWARE_ERROR
### 4. PAGE_TAB_HARDWARE_ERROR
### 5. ILLEGAL_COMMAND_ERROR
### 6. COMMAND_HARDWARE_ERROR
### 7. IOTLB_INV_TIMEOUT
### 8. INVALID_DEVICE_REQUEST
### 9. INVALID_PPR_REQUEST
### 10. EVENT_COUNTER_ZERO
### 11. GUEST_EVENT_FAULT
### 12. VIOMMU_HARDWARE_ERROR
### 13. RMP_PAGE_FAULT
### 14. RMP_HARDWARE_ERROR
## Interrupt Remapping
### MSI / MSI-X
To remap MSI the address need to be configured to point to `FD_F8xx_xxxx`.[Table 3 in specification.](^See)

## Registers
### PCI configuration space
All accesses *must* be 32-bits and aligned to a 32-bit boundary.
Software should write zeros to reserved bits.

#### Base Capability (id=0xf)
{| class="wikitable"
|-
! Offset
! Name
|-
| 0x00
| Capability Header
|-
| 0x04
| Base Address Low Register
|-
| 0x08
| Base Address High Register
|-
| 0x0C
| Range Register
|-
| 0x10
| Miscellaneous Information Register 0
|-
| 0x14
| Miscellaneous Information Register 1
|-
|}

### MMIO
All accesses *should* be 64-bits and properly aligned,
but smaller power-of-two accesses (32/16/8-bits) are also permitted.
Larger accesses (128/...-bits) are *not* permitted.

{| class="wikitable" style="table-layout:fixed"
! Offset
!style="width:50%"| Name (+4)
!style="width:50%"| Name (+0)
|-
!colspan="3"| 0x0000 to 0x1000
|-
| 0x0000
|colspan="2"| Device Table Base Address Register
|-
| 0x0008
|colspan="2"| Command Buffer Base Address Register
|-
| 0x0010
|colspan="2"| Event Log Base Address Register
|-
| 0x0018
|colspan="2"| IOMMU Control Register
|-
| 0x0020
|colspan="2"| IOMMU Exclusion Base Register / Completion Store Base Register
|-
| 0x0028
|colspan="2"| IOMMU Exclusion Range Limit Register / Completion Store Limit Register
|-
| 0x0030
|colspan="2"| IOMMU Extended Feature Register
|-
| 0x0038
|colspan="2"| PPR Log Base Address Register
|-
| 0x0040
|colspan="2"| IOMMU Hardware Event Upper Register
|-
| 0x0048
|colspan="2"| IOMMU Hardware Event Lower Register
|-
| 0x0050
|colspan="2"| Hardware Event Status Register
|-
| 0x0058
|colspan="2"| (unmapped)
|-
| 0x0060
|colspan="2"| SMI Filter Register 0
|-
| 0x0068
|colspan="2"| SMI Filter Register 1
|-
| 0x0070
|colspan="2"| SMI Filter Register 2
|-
| 0x0078
|colspan="2"| SMI Filter Register 3
|-
| 0x0080
|colspan="2"| SMI Filter Register 4
|-
| 0x0088
|colspan="2"| SMI Filter Register 5
|-
| 0x0090
|colspan="2"| SMI Filter Register 6
|-
| 0x0098
|colspan="2"| SMI Filter Register 7
|-
| 0x00A0
|colspan="2"| SMI Filter Register 8
|-
| 0x00A8
|colspan="2"| SMI Filter Register 9
|-
| 0x00B0
|colspan="2"| SMI Filter Register 10
|-
| 0x00B8
|colspan="2"| SMI Filter Register 11
|-
| 0x00C0
|colspan="2"| SMI Filter Register 12
|-
| 0x00C8
|colspan="2"| SMI Filter Register 13
|-
| 0x00D0
|colspan="2"| SMI Filter Register 14
|-
| 0x00D8
|colspan="2"| SMI Filter Register 15
|-
| 0x00E0
|colspan="2"| Guest Virtual APIC Log Base Address Register
|-
| 0x00E8
|colspan="2"| Guest Virtual APIC Log Tail Address Register
|-
| 0x00F0
|colspan="2"| PPR Log B Base Address Register
|-
| 0x00F8
|colspan="2"| Event Log B Base Address Register
|-
| 0x0100
|colspan="2"| Device Table Segment 1 Base Address Register
|-
| 0x0108
|colspan="2"| Device Table Segment 2 Base Address Register
|-
| 0x0110
|colspan="2"| Device Table Segment 3 Base Address Register
|-
| 0x0118
|colspan="2"| Device Table Segment 4 Base Address Register
|-
| 0x0120
|colspan="2"| Device Table Segment 5 Base Address Register
|-
| 0x0128
|colspan="2"| Device Table Segment 6 Base Address Register
|-
| 0x0130
|colspan="2"| Device Table Segment 7 Base Address Register
|-
| 0x0138
|colspan="2"| Device-Specific Feature Extension (DSFX) Register
|-
| 0x0140
|colspan="2"| Device-Specific Control Extension (DSCX) Register
|-
| 0x0148
|colspan="2"| Device-Specific Status Extension (DSSX) Register
|-
| 0x0150
| MSI Vector Register 1
| MSI Vector Register 0
|-
| 0x0158
| MSI Address Low Register
| MSI Capability Header Register
|-
| 0x0160
| MSI Data Register
| MSI Address High Register
|-
| 0x0168
| IOMMU Performance Optimization Control Register
| MSI Mapping Capability Header Register
|-
| 0x0170
|colspan="2"| XT IOMMU General Interrupt Control Register
|-
| 0x0178
|colspan="2"| XT IOMMU PPR Interrupt Control Register
|-
| 0x0180
|colspan="2"| XT IOMMU GA Log Interrupt Control Register
|-
| 0x0188
|colspan="2"| (reserved)
|-
| 0x0190
|colspan="2"| vIOMMU Status Register
|-
| 0x0198
|colspan="2"| (reserved)
|-
| 0x01A0
|colspan="2"| IOMMU Extended Feature 2 Register
|-
|colspan="3"| ...
|-
| 0x0200
|colspan="2"| MARC Aperture 0 Base Register
|-
| 0x0208
|colspan="2"| MARC Aperture 0 Relocation Register
|-
| 0x0210
|colspan="2"| MARC Aperture 0 Length Register
|-
| 0x0218
|colspan="2"| MARC Aperture 1 Base Register
|-
| 0x0220
|colspan="2"| MARC Aperture 1 Relocation Register
|-
| 0x0228
|colspan="2"| MARC Aperture 1 Length Register
|-
| 0x0230
|colspan="2"| MARC Aperture 2 Base Register
|-
| 0x0238
|colspan="2"| MARC Aperture 2 Relocation Register
|-
| 0x0240
|colspan="2"| MARC Aperture 2 Length Register
|-
| 0x0248
|colspan="2"| MARC Aperture 3 Base Register
|-
| 0x0250
|colspan="2"| MARC Aperture 3 Relocation Register
|-
| 0x0258
|colspan="2"| MARC Aperture 3 Length Register
|-
| 0x0260
|colspan="2"| (reserved)
|-
|colspan="3"| ...
|-
!colspan="3"| 0x1000 to 0x2000
|-
| 0x1000
|colspan="2"| (reserved)
|-
|colspan="3"| ...
|-
| 0x1FF8
|colspan="2"| IOMMU Reserved Register
|-
!colspan="3"| 0x2000 to 0x3000
|-
| 0x2000
|colspan="2"| Command Buffer Head Pointer Register
|-
| 0x2008
|colspan="2"| Command Buffer Tail Pointer Register
|-
| 0x2010
|colspan="2"| Event Log Head Pointer Register
|-
| 0x2018
|colspan="2"| Event Log Tail Pointer Register
|-
| 0x2020
|colspan="2"| IOMMU Status Register
|-
| 0x2028
|colspan="2"| (reserved)
|-
| 0x2030
|colspan="2"| IOMMU PPR Log Head Pointer Register
|-
| 0x2038
|colspan="2"| IOMMU PPR Log Tail Pointer Register
|-
| 0x2040
|colspan="2"| Guest Virtual APIC Log Head Pointer Register
|-
| 0x2048
|colspan="2"| Guest Virtual APIC Log Tail Pointer Register
|-
| 0x2050
|colspan="2"| PPR Log B Head Pointer Register
|-
| 0x2058
|colspan="2"| PPR Log B Tail Pointer Register
|-
| 0x2060
|colspan="2"| Event Log B Head Pointer Register
|-
| 0x2068
|colspan="2"| Event Log B Tail Pointer Register
|-
| 0x2070
|colspan="2"| Event Log B Head Pointer Register
|-
| 0x2078
|colspan="2"| Event Log B Tail Pointer Register
|-
| 0x2080
|colspan="2"| PPR Log Auto Response Register
|-
| 0x2088
|colspan="2"| PPR Log Overflow Early Indicator Register
|-
| 0x2090
|colspan="2"| PPR Log B Overflow Early Indicator Register
|-
| 0x2098
|colspan="2"| (reserved)
|}

## Troubleshooting
### QEMU: no DMA translation
DMA translation needs to explicitly enabled:

<pre>
-device amd-iommu,dma-remap=on
</pre>

Interrupt remapping, IOTLB and other features also need to be enabled explicitly. Run with `help` to see all options:

<pre>
-device amd-iommu,help
</pre>

## See also
<references />

### External Links
- [AMD I/O Virtualization Technology (IOMMU) Specification (48882-PUB—Rev 3.11—Apr 2026)](https://docs.amd.com/api/khub/documents/2oKZlc87KNCKiT3B~xJjHg/content)

[Virtualization](Category:X86)(Category:X86 Virtualization)
[Category:X86-64](Category:X86-64)
