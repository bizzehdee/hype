/*
 * #535: link a hype micro-kernel at its fixed load address.
 *
 * The address is not configurable: it is HYPE_KBOOT_LOAD_GPA from core/kboot.h, and the loader
 * places the payload there for every kernel-boot VM. A per-VM load address would mean re-linking
 * artifacts per config, which is the build/test coupling #534 exists to remove.
 */
ENTRY(_start)

SECTIONS {
    . = 0x1000000;

    .head : { *(.head) }
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }

    /*
     * NOLOAD: hype zeroes a guest's whole RAM before its first VM-entry (the M2-6 invariant), so
     * BSS is already zero and emitting it into the image would only make every artifact larger by
     * however much it declares.
     */
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

/*
 * The whole contract in one line: the 64-bit entry point is the load address plus 0x200
 * (hype_linux_64bit_entry). If .head ever stops being exactly 0x200 bytes, the guest is entered
 * mid-instruction and the failure is an exception with no explanation -- so fail the BUILD.
 */
ASSERT(_start == 0x1000200, "entry must land at load address + 0x200 -- check .head's size")
