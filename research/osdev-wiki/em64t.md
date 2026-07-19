---
title: 'EM64T'
source_page_id: '1895'
source_revision_id: '28117'
source_timestamp: '2023-07-09T23:15:46Z'
source_format: mediawiki
---

# EM64T

Intel Extended Memory 64 Technology ([Intel EM64T](http://www.intel.com/technology/64bitextensions/)) enables 64-bit computing on the server/workstation and desktop platforms when combined with supporting software. Intel EM64T improves performance by allowing the system to address more than 4 GB of both virtual and physical memory.

Technically speaking, EM64T offers to Intel processors what [x86-64](x86-64) offers to AMD processors. Ancient disagreements such as 3Dnow-vs-MMX still holds, but the rest should be exactly the same. It is supported by latest XEON processors.

Intel EM64T provides support for:

- 64-bit flat virtual address space
- 64-bit pointers
- 64-bit wide general purpose registers (16 - added R8-R15)
- 64-bit integer support
- Up to 1 tebibyte (TiB)  of platform address space

Currently physical addresses are restricted to 40 bits, and linear addresses are restricted to 48 bits, sign-extended.

## See Also
### Articles
- [X86-64](X86-64)
- [a 64-bit kernel](Creating)(Creating a 64-bit kernel)
- [up long mode](Setting)(User:Stephanvanschaik/Setting_Up_Long_Mode)

### External Links
- [EM64T](Wikipedia:EM64T) on Wikipedia.
- [XEON manual](ftp://download.intel.com/design/Pentium4/manuals/25366816.pdf)

[CPU](Category:X86)(Category:X86 CPU)
[Category:X86-64](Category:X86-64)
