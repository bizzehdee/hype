#include "kboot.h"

#include "linux_boot.h"

/* One 2MB granule of headroom above the kernel's own stated footprint, for early allocations
 * that init_size does not cover (and for the microtest artifacts, whose init_size IS the
 * payload). */
#define KBOOT_TAIL_HEADROOM (2ull * 1024ull * 1024ull)

uint64_t hype_kboot_min_ram_bytes(uint64_t payload_bytes, uint64_t init_size,
                                  uint64_t initrd_bytes) {
    uint64_t footprint = (init_size > payload_bytes) ? init_size : payload_bytes;
    return HYPE_KBOOT_LOAD_GPA + footprint + KBOOT_TAIL_HEADROOM + initrd_bytes;
}

/* #545: see the header. The map a real bootloader would report for this layout. */
unsigned int hype_kboot_build_e820(uint64_t ram_bytes, void *out_entries) {
    hype_linux_e820_entry_t *e = (hype_linux_e820_entry_t *)out_entries;
    unsigned int n = 0;

    /* Page 0: real-mode IVT/BDA territory; no bootloader reports it usable. */
    e[n].addr = 0x0ull;
    e[n].size = 0x1000ull;
    e[n].type = HYPE_LINUX_E820_TYPE_RESERVED;
    n++;
    /* The guest's page tables (0x1000-0x6FFF), the zero page (0x7000) and the command line
     * (0x8000). The kernel switches CR3 early, but the zero page being clobbered before it is
     * copied is a real race, and "the map is honest" holds independently of who gets away with
     * what. */
    e[n].addr = 0x1000ull;
    e[n].size = HYPE_KBOOT_CMDLINE_GPA + 0x1000ull - 0x1000ull;
    e[n].type = HYPE_LINUX_E820_TYPE_RESERVED;
    n++;
    /* Usable low RAM up to the PC hole (the guest stack lives here -- conventionally usable,
     * exactly as GRUB's own stack is). */
    e[n].addr = HYPE_KBOOT_CMDLINE_GPA + 0x1000ull;
    e[n].size = 0xA0000ull - (HYPE_KBOOT_CMDLINE_GPA + 0x1000ull);
    e[n].type = HYPE_LINUX_E820_TYPE_RAM;
    n++;
    /* The PC hole: VGA, option-ROM and BIOS shadow space. Nothing of hype's lives there, but no
     * PC-shaped map calls it RAM. */
    e[n].addr = 0xA0000ull;
    e[n].size = 0x100000ull - 0xA0000ull;
    e[n].type = HYPE_LINUX_E820_TYPE_RESERVED;
    n++;
    if (ram_bytes > 0x100000ull) {
        e[n].addr = 0x100000ull;
        e[n].size = ram_bytes - 0x100000ull;
        e[n].type = HYPE_LINUX_E820_TYPE_RAM;
        n++;
    }
    return n;
}

hype_kboot_status_t hype_kboot_plan(const void *image_head, uint64_t head_bytes,
                                    uint64_t image_bytes, uint64_t ram_bytes,
                                    unsigned int cmdline_len, int have_cmdline,
                                    uint64_t initrd_bytes, hype_kboot_plan_t *out) {
    const unsigned char *head = (const unsigned char *)image_head;
    const hype_linux_setup_header_t *hdr;
    uint32_t payload_offset;
    uint64_t payload_bytes;
    uint64_t need;

    if (head == 0 || out == 0) {
        return HYPE_KBOOT_ERR_BAD_HEADER;
    }
    if (head_bytes < HYPE_LINUX_SETUP_HEADER_OFFSET + sizeof(hype_linux_setup_header_t)) {
        return HYPE_KBOOT_ERR_HEAD_TOO_SMALL;
    }
    if (image_bytes < HYPE_LINUX_SETUP_HEADER_OFFSET + sizeof(hype_linux_setup_header_t)) {
        return HYPE_KBOOT_ERR_SHORT_IMAGE;
    }

    hdr = (const hype_linux_setup_header_t *)(head + HYPE_LINUX_SETUP_HEADER_OFFSET);
    if (!hype_linux_header_is_valid(hdr)) {
        return HYPE_KBOOT_ERR_BAD_HEADER;
    }

    payload_offset = hype_linux_payload_file_offset(hdr);
    if ((uint64_t)payload_offset >= image_bytes) {
        return HYPE_KBOOT_ERR_NO_PAYLOAD;
    }
    payload_bytes = image_bytes - (uint64_t)payload_offset;

    need = hype_kboot_min_ram_bytes(payload_bytes, hdr->init_size, initrd_bytes);
    if (ram_bytes < need) {
        return HYPE_KBOOT_ERR_RAM_TOO_SMALL;
    }

    /* #546: refuse before anything is placed, so a too-long command line costs one VM and not a
     * half-loaded guest. Checked against the reserved page and against the kernel's own stated
     * limit, whichever is smaller -- cmdline_size 0 means the image did not state one. */
    if (have_cmdline) {
        if (cmdline_len > HYPE_KBOOT_CMDLINE_MAX) {
            return HYPE_KBOOT_ERR_CMDLINE_TOO_LONG;
        }
        if (hdr->cmdline_size != 0u && cmdline_len > hdr->cmdline_size) {
            return HYPE_KBOOT_ERR_CMDLINE_TOO_LONG;
        }
    }

    out->payload_file_offset = payload_offset;
    out->payload_bytes = payload_bytes;
    out->payload_load_gpa = HYPE_KBOOT_LOAD_GPA;
    out->entry_gpa = hype_linux_64bit_entry(HYPE_KBOOT_LOAD_GPA);
    out->cr3_gpa = HYPE_KBOOT_PML4_GPA;
    out->rsp_gpa = HYPE_KBOOT_STACK_TOP_GPA;
    out->zero_page_gpa = HYPE_KBOOT_ZERO_PAGE_GPA;
    out->gb_to_map = HYPE_KBOOT_PD_PAGES;
    out->cmdline_gpa = have_cmdline ? HYPE_KBOOT_CMDLINE_GPA : 0ull;
    out->initrd_gpa = 0ull;
    out->initrd_bytes = 0ull;
    if (initrd_bytes != 0ull) {
        /*
         * #545: as high as the kernel's own initrd_addr_max and this VM's RAM allow, page-aligned
         * down -- the placement every real bootloader uses. initrd_addr_max is the LAST byte the
         * kernel can address, so the window ends at initrd_addr_max + 1; 0 means the image
         * predates the field (protocol < 2.03), for which the kernel's documented limit is
         * 0x37FFFFFF -- but hype_linux_header_is_valid() already requires 2.10+, so 0 here is a
         * broken image and the conservative window handles it anyway.
         */
        uint64_t ceiling = (uint64_t)hdr->initrd_addr_max + 1ull;
        uint64_t window_end = (ram_bytes < ceiling) ? ram_bytes : ceiling;
        uint64_t kernel_top = HYPE_KBOOT_LOAD_GPA +
                              ((hdr->init_size > payload_bytes) ? hdr->init_size : payload_bytes) +
                              KBOOT_TAIL_HEADROOM;
        uint64_t gpa;
        if (window_end < initrd_bytes) {
            return HYPE_KBOOT_ERR_INITRD_UNREACHABLE;
        }
        gpa = (window_end - initrd_bytes) & ~0xFFFull;
        if (gpa < kernel_top) {
            /* Below the kernel's decompression scratch: the initrd would be overwritten before
             * the kernel ever looked at it. Refuse with the limit named by the caller's log. */
            return HYPE_KBOOT_ERR_INITRD_UNREACHABLE;
        }
        out->initrd_gpa = gpa;
        out->initrd_bytes = initrd_bytes;
    }
    return HYPE_KBOOT_OK;
}

const char *hype_kboot_status_str(hype_kboot_status_t s) {
    switch (s) {
        case HYPE_KBOOT_OK:
            return "ok";
        case HYPE_KBOOT_ERR_SHORT_IMAGE:
            return "the file is too small to be a kernel image";
        case HYPE_KBOOT_ERR_BAD_HEADER:
            return "not a 64-bit-capable bzImage (setup header magic, version or XLF_KERNEL_64)";
        case HYPE_KBOOT_ERR_NO_PAYLOAD:
            return "the setup header is valid but no payload follows it";
        case HYPE_KBOOT_ERR_RAM_TOO_SMALL:
            return "the payload does not fit in this VM's mem_mb";
        case HYPE_KBOOT_ERR_HEAD_TOO_SMALL:
            return "too few header bytes were read to decide";
        case HYPE_KBOOT_ERR_CMDLINE_TOO_LONG:
            return "the cmdline is longer than the layout or than the kernel accepts";
        case HYPE_KBOOT_ERR_INITRD_UNREACHABLE:
            return "the initrd cannot be placed below the image's initrd_addr_max and above the "
                   "kernel's decompression scratch";
        default:
            return "unknown";
    }
}
