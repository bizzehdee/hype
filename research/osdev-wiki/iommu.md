---
title: 'IOMMU'
source_page_id: '5001'
source_revision_id: '30116'
source_timestamp: '2026-03-03T15:35:16Z'
source_format: mediawiki
---

# IOMMU

> This page or section is a stub.

An **IOMMU** (Input Output Memory Management Unit) implements a form of Virtual Memory through [Paging](Paging), for I/O devices. This means that their [DMA](DMA) transactions can be access-controlled, and remapped, instead of directly accessing physical memory.

## Uses
IOMMUs have more than one use. The most obvious deployment is using a [Hypervisor](Hypervisor). Guest OSes in virtual machines can then access virtualized hardware that is passed-through to the guest VM, but the IOMMU will remap Guest Physical Addresses to the real memory backing it, and prevent DMA accesses beyond the bounds of the VM.

However, the same principles may also be applied to application software running in an unprivileged context, too. Some IOMMUs use the same paging format at the CPU, and nested translation too. This means they can translate Virtual Addresses, to Guest Physical Address, to Physical Addresses. Using this, it is possible to allow Ring 3 software to communicate directly with the hardware, and program DMA, without breaking security. Some modern graphics cards support this for performance reasons, for instance.

## Implementations
### X86
- [IOMMU](AMD-Vi)(AMD-Vi IOMMU)
- [VT-d](Intel)(Intel VT-d)

### ARM
- [SMMU versions 1 and 2](ARM)(ARM SMMU versions 1 and 2)

[Category:Virtualization](Category:Virtualization)
