#include "linux_boot.h"

int hype_linux_header_is_valid(const hype_linux_setup_header_t *hdr) {
    if (hdr->boot_flag != HYPE_LINUX_BOOT_FLAG) {
        return 0;
    }
    if (hdr->header != HYPE_LINUX_HDR_MAGIC) {
        return 0;
    }
    if (hdr->version < HYPE_LINUX_MIN_VERSION) {
        return 0;
    }
    if ((hdr->xloadflags & HYPE_LINUX_XLF_KERNEL_64) == 0) {
        return 0;
    }
    return 1;
}

uint32_t hype_linux_payload_file_offset(const hype_linux_setup_header_t *hdr) {
    uint32_t setup_sects = hdr->setup_sects;
    if (setup_sects == 0) {
        setup_sects = 4;
    }
    return (setup_sects + 1u) * 512u;
}

uint64_t hype_linux_64bit_entry(uint64_t payload_load_address) {
    return payload_load_address + 0x200u;
}

void hype_linux_build_zero_page(hype_linux_boot_params_t *params, const hype_linux_setup_header_t *hdr,
                                 uint32_t ramdisk_image, uint32_t ramdisk_size, uint32_t cmd_line_ptr,
                                 const hype_linux_e820_entry_t *e820_entries, uint8_t e820_count) {
    unsigned char *bytes = (unsigned char *)params;
    unsigned long long i;
    uint8_t clamped_count;

    for (i = 0; i < sizeof(*params); i++) {
        bytes[i] = 0;
    }

    params->hdr = *hdr;
    params->hdr.type_of_loader = HYPE_LINUX_TYPE_OF_LOADER_UNDEFINED;
    params->hdr.loadflags = HYPE_LINUX_LOADFLAGS_LOADED_HIGH;
    params->hdr.ramdisk_image = ramdisk_image;
    params->hdr.ramdisk_size = ramdisk_size;
    params->hdr.cmd_line_ptr = cmd_line_ptr;

    clamped_count = e820_count;
    if (clamped_count > HYPE_LINUX_E820_MAX_ENTRIES) {
        clamped_count = HYPE_LINUX_E820_MAX_ENTRIES;
    }
    for (i = 0; i < clamped_count; i++) {
        params->e820_table[i] = e820_entries[i];
    }
    params->e820_entries = clamped_count;
}
