#include "kboot.h"

#include "linux_boot.h"

/* One 2MB granule of headroom above the payload, so the kernel has room to place its own
 * decompressed image, BSS and early allocations without hitting the top of RAM. */
#define KBOOT_TAIL_HEADROOM (2ull * 1024ull * 1024ull)

uint64_t hype_kboot_min_ram_bytes(uint64_t payload_bytes) {
    return HYPE_KBOOT_LOAD_GPA + payload_bytes + KBOOT_TAIL_HEADROOM;
}

hype_kboot_status_t hype_kboot_plan(const void *image_head, uint64_t head_bytes,
                                    uint64_t image_bytes, uint64_t ram_bytes,
                                    unsigned int cmdline_len, int have_cmdline,
                                    hype_kboot_plan_t *out) {
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

    need = hype_kboot_min_ram_bytes(payload_bytes);
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
        default:
            return "unknown";
    }
}
