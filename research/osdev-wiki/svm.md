---
title: 'SVM'
source_page_id: '5006'
source_revision_id: '30150'
source_timestamp: '2026-03-06T10:57:12Z'
source_format: mediawiki
---

# SVM

> This page or section is a stub.

**AMD's Secure Virtual Machine** is AMD's counterpart to Intel's [VMX](VMX) instructions.

## Programming
### Discovering support
After enumerating the features from [CPUID](CPUID), page 0x80000001, bit 2 in ECX indicates support for SVM.

Next, the VM_CR MSR (0xC00010114) needs to be consulted. Bit 4, SVMDIS, must be zero. If set, the CPU supports SVM, but it has been disabled by the platform firmware.


### Basic environment
Software should enable bit 12 of EFER (EFER.SVME) first.

## References
[Virtualization](Category:X86)(Category:X86 Virtualization)
