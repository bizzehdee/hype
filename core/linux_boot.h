#ifndef HYPE_CORE_LINUX_BOOT_H
#define HYPE_CORE_LINUX_BOOT_H

#include <stdint.h>

/*
 * Linux/x86_64 direct-boot protocol (M3-3): the "shim" half of
 * booting a raw bzImage directly, with no guest firmware and no
 * guest-visible real-mode bootloader stage at all -- the same
 * direct-boot path QEMU's `-kernel`, Firecracker, and crosvm all use.
 * Struct layouts/offsets below are transcribed from the Linux kernel's
 * own Documentation/arch/x86/boot.rst and
 * arch/x86/include/uapi/asm/bootparam.h (current upstream, looked up
 * for this task rather than reconstructed from memory alone, given a
 * wrong offset here is a hardware/ABI-layout bug -- same rigor as
 * vmcb.h/vmcs_fields.h). This project only ever boots x86_64 guests
 * (project decision, no 32-bit guest support), so only the 64-bit
 * entry protocol is implemented -- a kernel without 64-bit entry
 * support is unsupported, not a degraded fallback.
 *
 * Scoped to parsing/construction logic alone, pure and testable. Full
 * end-to-end launch (actually VMRUNning a kernel through this) is
 * M3-5's job, once M3-4's device stubs exist too.
 */

/* setup_header, embedded in the bzImage file (and later in
 * boot_params) at offset 0x1F1. Only fields this loader reads/writes
 * are given real names; everything else is a named-but-unused
 * reserved gap so later fields' offsets stay correct without claiming
 * certainty about bytes this project never touches. */
typedef struct {
    /* 0x1F1 */ uint8_t setup_sects;
    /* 0x1F2 */ uint8_t reserved_0x1F2[0x1F4 - 0x1F2];
    /* 0x1F4 */ uint32_t syssize;
    /* 0x1F8 */ uint8_t reserved_0x1F8[0x1FE - 0x1F8];
    /* 0x1FE */ uint16_t boot_flag;
    /* 0x200 */ uint8_t reserved_0x200[0x202 - 0x200];
    /* 0x202 */ uint32_t header;
    /* 0x206 */ uint16_t version;
    /* 0x208 */ uint8_t reserved_0x208[0x210 - 0x208];
    /* 0x210 */ uint8_t type_of_loader;
    /* 0x211 */ uint8_t loadflags;
    /* 0x212 */ uint8_t reserved_0x212[0x218 - 0x212];
    /* 0x218 */ uint32_t ramdisk_image;
    /* 0x21C */ uint32_t ramdisk_size;
    /* 0x220 */ uint8_t reserved_0x220[0x228 - 0x220];
    /* 0x228 */ uint32_t cmd_line_ptr;
    /* 0x22C */ uint32_t initrd_addr_max;
    /* 0x230 */ uint32_t kernel_alignment;
    /* 0x234 */ uint8_t reserved_0x234[0x236 - 0x234];
    /* 0x236 */ uint16_t xloadflags;
    /* 0x238 */ uint32_t cmdline_size;
    /* 0x23C */ uint8_t reserved_0x23C[0x258 - 0x23C];
    /* 0x258 */ uint64_t pref_address;
    /* 0x260 */ uint32_t init_size;
    /* 0x264 */ uint8_t reserved_0x264[0x270 - 0x264];
} __attribute__((packed)) hype_linux_setup_header_t;

_Static_assert(sizeof(hype_linux_setup_header_t) == (0x270 - 0x1F1),
               "setup_header must span exactly file offsets 0x1F1-0x270");

#define HYPE_LINUX_BOOT_FLAG 0xAA55u
#define HYPE_LINUX_HDR_MAGIC 0x53726448u /* "HdrS" */
/* 2.10 -- chosen so xloadflags (added in 2.09/2.10) is always present
 * to check; the 64-bit entry protocol itself is what
 * HYPE_LINUX_XLF_KERNEL_64 confirms, not the version number alone. */
#define HYPE_LINUX_MIN_VERSION 0x020Au
#define HYPE_LINUX_XLF_KERNEL_64 (1u << 0)
#define HYPE_LINUX_LOADFLAGS_LOADED_HIGH (1u << 0)
#define HYPE_LINUX_TYPE_OF_LOADER_UNDEFINED 0xFFu

#define HYPE_LINUX_SETUP_HEADER_OFFSET 0x1F1u

/*
 * Validates a bzImage's embedded setup header: boot_flag == 0xAA55,
 * header == "HdrS", version >= 2.10, and the 64-bit entry protocol
 * bit (xloadflags & XLF_KERNEL_64) is set. Returns 1 if valid, 0
 * otherwise. Pure -- reads only the given header, no I/O.
 */
int hype_linux_header_is_valid(const hype_linux_setup_header_t *hdr);

/*
 * Where the (non-real-mode) kernel payload starts within the bzImage
 * file, in bytes: (setup_sects + 1) * 512, with setup_sects == 0
 * meaning 4 per the documented convention (skips the real-mode boot
 * sector and setup code entirely -- this loader never executes any of
 * it). Pure arithmetic.
 */
uint32_t hype_linux_payload_file_offset(const hype_linux_setup_header_t *hdr);

/*
 * The 64-bit entry point's guest-physical address, given where the
 * payload (from hype_linux_payload_file_offset()) was loaded into
 * guest memory: load_address + 0x200, per the documented 64-bit boot
 * protocol ("jumping to the 64-bit kernel entry point, which is the
 * start address of the loaded 64-bit kernel plus 0x200"). Pure
 * arithmetic.
 */
uint64_t hype_linux_64bit_entry(uint64_t payload_load_address);

/* One E820 memory-map entry, exactly as boot_params.e820_table
 * expects it (20 bytes, no padding). */
typedef struct {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
} __attribute__((packed)) hype_linux_e820_entry_t;

_Static_assert(sizeof(hype_linux_e820_entry_t) == 20, "E820 entry must be exactly 20 bytes");

#define HYPE_LINUX_E820_MAX_ENTRIES 128
#define HYPE_LINUX_E820_TYPE_RAM 1u

/* struct boot_params (the "zero page"): exactly one 4KB page. Only
 * the fields this loader actually fills are named; everything else
 * (screen_info, apm_bios_info, edd data, ...) is reserved padding this
 * project never reads or writes, left zeroed. */
typedef struct {
    /* 0x000 */ uint8_t reserved_0x000[0x1E8 - 0x000];
    /* 0x1E8 */ uint8_t e820_entries;
    /* 0x1E9 */ uint8_t reserved_0x1E9[0x1F1 - 0x1E9];
    /* 0x1F1 */ hype_linux_setup_header_t hdr;
    /* 0x270 */ uint8_t reserved_0x270[0x2D0 - 0x270];
    /* 0x2D0 */ hype_linux_e820_entry_t e820_table[HYPE_LINUX_E820_MAX_ENTRIES];
    /* 0xCD0 */ uint8_t reserved_0xCD0[0x1000 - 0xCD0];
} __attribute__((packed)) hype_linux_boot_params_t;

_Static_assert(sizeof(hype_linux_boot_params_t) == 0x1000,
               "boot_params (the \"zero page\") must be exactly one 4KB page");

/*
 * Builds a minimal zero page: zeroes `params` first (every guest gets
 * a clean zero page -- stale fields here are exactly the kind of
 * cross-boot leak M2-6's guest-RAM-zeroing invariant exists to
 * prevent), copies `hdr` into params->hdr, then overrides
 * type_of_loader (HYPE_LINUX_TYPE_OF_LOADER_UNDEFINED -- this project
 * isn't a recognized bootloader with an assigned ID) and loadflags
 * (sets LOADED_HIGH, since the payload is always loaded at/above 1MB
 * here), and fills in ramdisk_image/ramdisk_size (0 for "no initrd"),
 * cmd_line_ptr (0 for "no command line"), and up to
 * HYPE_LINUX_E820_MAX_ENTRIES E820 entries (e820_count is clamped, not
 * overflowed, if the caller passes more). Pure struct-filling -- no
 * CPU state touched, no UEFI dependency.
 */
void hype_linux_build_zero_page(hype_linux_boot_params_t *params, const hype_linux_setup_header_t *hdr,
                                 uint32_t ramdisk_image, uint32_t ramdisk_size, uint32_t cmd_line_ptr,
                                 const hype_linux_e820_entry_t *e820_entries, uint8_t e820_count);

#endif /* HYPE_CORE_LINUX_BOOT_H */
