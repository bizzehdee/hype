#!/usr/bin/env python3
"""#535: wrap a flat micro-kernel payload in a bzImage-shaped setup region.

hype's `boot = kernel` loader validates a real Linux/x86_64 setup header
(core/linux_boot.c) and derives the payload's file offset from it. That is
deliberate: the microtest artifacts go in through the SAME path a real kernel
would, so the path is exercised by every test rather than by one special case.

The output is:

    [ 2560-byte setup region, with the setup header at file offset 0x1F1 ]
    [ the payload, verbatim                                             ]

setup_sects is 4, so hype_linux_payload_file_offset() returns (4+1)*512 = 2560
and the payload begins exactly where this script put it. The real-mode boot
sector and setup code a genuine bzImage carries are absent -- hype's loader
never executes them, and a stub that is never run is better left out than
faked.
"""

import struct
import sys

SETUP_SECTS = 4
SETUP_BYTES = (SETUP_SECTS + 1) * 512
HDR_OFFSET = 0x1F1

# core/linux_boot.h's own constants, and the reason each is here.
BOOT_FLAG = 0xAA55           # offset 0x1FE -- the boot sector signature
HDR_MAGIC = 0x53726448       # offset 0x202 -- "HdrS"
VERSION = 0x020F             # offset 0x206 -- >= 2.10, so xloadflags exists
XLF_KERNEL_64 = 1 << 0       # offset 0x236 -- 64-bit entry; hype requires it
LOAD_GPA = 0x1000000         # HYPE_KBOOT_LOAD_GPA, reported as pref_address


def build(payload: bytes) -> bytes:
    setup = bytearray(SETUP_BYTES)

    def put(off, fmt, val):
        struct.pack_into(fmt, setup, off, val)

    put(0x1F1, "<B", SETUP_SECTS)
    put(0x1F4, "<I", (len(payload) + 15) // 16)  # syssize, in 16-byte paragraphs
    put(0x1FE, "<H", BOOT_FLAG)
    put(0x202, "<I", HDR_MAGIC)
    put(0x206, "<H", VERSION)
    put(0x230, "<I", 0x200000)                   # kernel_alignment (2 MB)
    put(0x236, "<H", XLF_KERNEL_64)
    put(0x258, "<Q", LOAD_GPA)                   # pref_address
    put(0x260, "<I", len(payload))               # init_size
    return bytes(setup) + payload


def main(argv):
    if len(argv) != 3:
        print("usage: mkbzimage.py <payload.bin> <out.bin>", file=sys.stderr)
        return 2
    with open(argv[1], "rb") as f:
        payload = f.read()
    if not payload:
        print("mkbzimage: payload is empty", file=sys.stderr)
        return 1
    image = build(payload)
    with open(argv[2], "wb") as f:
        f.write(image)
    print("mkbzimage: %s -> %s (%d B setup + %d B payload = %d B)"
          % (argv[1], argv[2], SETUP_BYTES, len(payload), len(image)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
