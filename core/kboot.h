#ifndef HYPE_CORE_KBOOT_H
#define HYPE_CORE_KBOOT_H

#include <stdint.h>

/*
 * #535 (plan.md §10 decision 45): planning half of `boot = kernel` -- a configured VM that
 * boots a raw guest kernel image with no guest firmware in the path.
 *
 * core/linux_boot.h already validates a bzImage's setup header, says where the payload starts
 * and where its 64-bit entry point lands. What it does not do is decide WHERE in a VM's guest
 * RAM the payload, the zero page, the stack and the guest's own page tables go, or refuse an
 * image that will not fit. That is this module: pure arithmetic over the header bytes and the
 * VM's RAM size, so the whole "will this load, and where" question is unit-testable without a
 * volume, a VM or a CPU.
 *
 * The guest-physical layout below is fixed rather than computed. A microtest kernel is built
 * against it (its linker script names HYPE_KBOOT_LOAD_GPA), and a layout that moved per VM
 * would mean re-linking artifacts per config -- which is exactly the build/test coupling #534
 * exists to remove.
 */

/* Guest page tables: PML4, PDPT, then HYPE_KBOOT_PD_PAGES consecutive PD pages (one per GB). */
#define HYPE_KBOOT_PML4_GPA 0x1000ull
#define HYPE_KBOOT_PDPT_GPA 0x2000ull
#define HYPE_KBOOT_PD0_GPA 0x3000ull
/*
 * 4 GB identity-mapped, because the guest's own devices are not all in its RAM: the local APIC
 * at 0xFEE00000, the I/O APIC at 0xFEC00000, ECAM and the flash window all sit in the low 4 GB
 * of guest-physical space. A kernel mapping only its RAM would fault the moment it touched one.
 */
#define HYPE_KBOOT_PD_PAGES 4u
#define HYPE_KBOOT_ZERO_PAGE_GPA 0x7000ull
/*
 * #546: the kernel command line, one page immediately after the zero page. The kernel finds it
 * through boot_params.hdr.cmd_line_ptr, so its address is not something a guest hardcodes -- it is
 * here only so the layout has one owner.
 */
#define HYPE_KBOOT_CMDLINE_GPA 0x8000ull
#define HYPE_KBOOT_CMDLINE_MAX 4095u /* one page, less the NUL */
/*
 * Stack top at 512 KB, growing down toward the command-line page. Below the 640 KB
 * conventional-memory ceiling, so it cannot collide with anything a PC-shaped guest expects to
 * find in the 0xA0000-0xFFFFF hole. That leaves 0x77000 (476 KB) of stack before it would reach
 * the command line -- far more than a test kernel uses, and stated here because a silent collision
 * between a deep stack and the command line would corrupt the one string that says what the guest
 * was asked to do.
 */
#define HYPE_KBOOT_STACK_TOP_GPA 0x80000ull
/* Payload at 16 MB: clear of the low 1 MB entirely, and 2MB-aligned. */
#define HYPE_KBOOT_LOAD_GPA 0x1000000ull

/* Bytes of the image head hype_kboot_plan() needs to see (setup header + slack). */
#define HYPE_KBOOT_HEAD_BYTES 4096u

typedef enum {
    HYPE_KBOOT_OK = 0,
    HYPE_KBOOT_ERR_SHORT_IMAGE,   /* the file is too small to contain a setup header */
    HYPE_KBOOT_ERR_BAD_HEADER,    /* not a bzImage, or 32-bit-only (no XLF_KERNEL_64) */
    HYPE_KBOOT_ERR_NO_PAYLOAD,    /* header is valid but nothing follows the setup region */
    HYPE_KBOOT_ERR_RAM_TOO_SMALL, /* the payload will not fit in this VM's guest RAM */
    HYPE_KBOOT_ERR_HEAD_TOO_SMALL, /* caller passed fewer head bytes than the header needs */
    HYPE_KBOOT_ERR_CMDLINE_TOO_LONG /* #546: longer than the layout, or than the kernel accepts */
} hype_kboot_status_t;

typedef struct {
    uint64_t entry_gpa;        /* where to set guest RIP */
    uint64_t cr3_gpa;          /* guest CR3 -- HYPE_KBOOT_PML4_GPA */
    uint64_t rsp_gpa;          /* guest RSP */
    uint64_t zero_page_gpa;    /* what RSI must hold at entry */
    uint64_t payload_load_gpa; /* where the payload bytes go */
    uint32_t payload_file_offset; /* first payload byte's offset in the image file */
    uint64_t payload_bytes;    /* how many bytes to copy from there */
    unsigned int gb_to_map;    /* GB of identity map to build */
    /*
     * #546: where the command line goes, and 0 for cmd_line_ptr when there is none. The kernel
     * reads cmd_line_ptr, so 0 means "no command line" to it -- distinct from an empty string at a
     * valid address, which means "an empty command line". Both are legitimate; they are not the
     * same thing, and conflating them would make `cmdline =` indistinguishable from no key.
     */
    uint64_t cmdline_gpa;
} hype_kboot_plan_t;

/*
 * Validates the image head and fills `out` with everything the loader needs. `image_head` must
 * hold at least HYPE_KBOOT_HEAD_BYTES bytes read from file offset 0; `image_bytes` is the whole
 * file's size; `ram_bytes` is the VM's guest RAM. `out` is untouched unless the result is
 * HYPE_KBOOT_OK.
 */
/*
 * `cmdline_len` is the command line's length in bytes excluding any NUL, or 0 for none;
 * `have_cmdline` distinguishes "no command line at all" from "an empty one". The length is checked
 * against BOTH the page this layout reserves and, when the image declares one, the kernel's own
 * cmdline_size -- which is the kernel telling the loader how much it will read. A longer string is
 * refused rather than truncated: a truncated command line silently means something ELSE, and
 * `console=ttyS0` cut to `console=tty` is a valid, wrong setting.
 */
hype_kboot_status_t hype_kboot_plan(const void *image_head, uint64_t head_bytes,
                                    uint64_t image_bytes, uint64_t ram_bytes,
                                    unsigned int cmdline_len, int have_cmdline,
                                    hype_kboot_plan_t *out);

/* The refusal reason, so every caller words it the same way. */
const char *hype_kboot_status_str(hype_kboot_status_t s);

/*
 * The smallest guest RAM that can hold a payload of `payload_bytes` at this layout. Reported
 * alongside a HYPE_KBOOT_ERR_RAM_TOO_SMALL refusal so the operator is told what to set mem_mb
 * to, rather than only that the current value is wrong.
 */
uint64_t hype_kboot_min_ram_bytes(uint64_t payload_bytes);

#endif /* HYPE_CORE_KBOOT_H */
