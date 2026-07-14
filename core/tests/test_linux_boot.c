#include <stdio.h>
#include "../linux_boot.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static hype_linux_setup_header_t valid_header(void) {
    hype_linux_setup_header_t hdr;
    unsigned char *bytes = (unsigned char *)&hdr;
    unsigned long long i;

    for (i = 0; i < sizeof(hdr); i++) {
        bytes[i] = 0;
    }
    hdr.setup_sects = 30;
    hdr.boot_flag = HYPE_LINUX_BOOT_FLAG;
    hdr.header = HYPE_LINUX_HDR_MAGIC;
    hdr.version = 0x020F;
    hdr.xloadflags = HYPE_LINUX_XLF_KERNEL_64;
    return hdr;
}

static void test_struct_sizes(void) {
    CHECK_HEX("setup_header spans file offsets 0x1F1-0x270", 0x270 - 0x1F1,
              sizeof(hype_linux_setup_header_t));
    CHECK_HEX("E820 entry is exactly 20 bytes", 20, sizeof(hype_linux_e820_entry_t));
    CHECK_HEX("boot_params is exactly one 4KB page", 0x1000, sizeof(hype_linux_boot_params_t));
}

static void test_field_offsets(void) {
    hype_linux_setup_header_t hdr;
    unsigned char *base = (unsigned char *)&hdr;

    CHECK_HEX("setup_sects at file offset 0x1F1", (unsigned long long)base,
              (unsigned long long)&hdr.setup_sects);
    CHECK_HEX("syssize at file offset 0x1F4", (unsigned long long)(base + (0x1F4 - 0x1F1)),
              (unsigned long long)&hdr.syssize);
    CHECK_HEX("boot_flag at file offset 0x1FE", (unsigned long long)(base + (0x1FE - 0x1F1)),
              (unsigned long long)&hdr.boot_flag);
    CHECK_HEX("header at file offset 0x202", (unsigned long long)(base + (0x202 - 0x1F1)),
              (unsigned long long)&hdr.header);
    CHECK_HEX("version at file offset 0x206", (unsigned long long)(base + (0x206 - 0x1F1)),
              (unsigned long long)&hdr.version);
    CHECK_HEX("type_of_loader at file offset 0x210", (unsigned long long)(base + (0x210 - 0x1F1)),
              (unsigned long long)&hdr.type_of_loader);
    CHECK_HEX("loadflags at file offset 0x211", (unsigned long long)(base + (0x211 - 0x1F1)),
              (unsigned long long)&hdr.loadflags);
    CHECK_HEX("ramdisk_image at file offset 0x218", (unsigned long long)(base + (0x218 - 0x1F1)),
              (unsigned long long)&hdr.ramdisk_image);
    CHECK_HEX("ramdisk_size at file offset 0x21C", (unsigned long long)(base + (0x21C - 0x1F1)),
              (unsigned long long)&hdr.ramdisk_size);
    CHECK_HEX("cmd_line_ptr at file offset 0x228", (unsigned long long)(base + (0x228 - 0x1F1)),
              (unsigned long long)&hdr.cmd_line_ptr);
    CHECK_HEX("initrd_addr_max at file offset 0x22C", (unsigned long long)(base + (0x22C - 0x1F1)),
              (unsigned long long)&hdr.initrd_addr_max);
    CHECK_HEX("kernel_alignment at file offset 0x230", (unsigned long long)(base + (0x230 - 0x1F1)),
              (unsigned long long)&hdr.kernel_alignment);
    CHECK_HEX("xloadflags at file offset 0x236", (unsigned long long)(base + (0x236 - 0x1F1)),
              (unsigned long long)&hdr.xloadflags);
    CHECK_HEX("cmdline_size at file offset 0x238", (unsigned long long)(base + (0x238 - 0x1F1)),
              (unsigned long long)&hdr.cmdline_size);
    CHECK_HEX("pref_address at file offset 0x258", (unsigned long long)(base + (0x258 - 0x1F1)),
              (unsigned long long)&hdr.pref_address);
    CHECK_HEX("init_size at file offset 0x260", (unsigned long long)(base + (0x260 - 0x1F1)),
              (unsigned long long)&hdr.init_size);
}

static void test_zero_page_field_offsets(void) {
    hype_linux_boot_params_t params;
    unsigned char *base = (unsigned char *)&params;

    CHECK_HEX("e820_entries at 0x1E8", (unsigned long long)(base + 0x1E8),
              (unsigned long long)&params.e820_entries);
    CHECK_HEX("hdr at 0x1F1", (unsigned long long)(base + 0x1F1), (unsigned long long)&params.hdr);
    CHECK_HEX("e820_table at 0x2D0", (unsigned long long)(base + 0x2D0),
              (unsigned long long)&params.e820_table);
}

static void test_header_is_valid_accepts_good_header(void) {
    hype_linux_setup_header_t hdr = valid_header();
    CHECK_HEX("a well-formed 64-bit-capable header is valid", 1, hype_linux_header_is_valid(&hdr));
}

static void test_header_is_valid_rejects_bad_boot_flag(void) {
    hype_linux_setup_header_t hdr = valid_header();
    hdr.boot_flag = 0x1234;
    CHECK_HEX("wrong boot_flag is rejected", 0, hype_linux_header_is_valid(&hdr));
}

static void test_header_is_valid_rejects_bad_magic(void) {
    hype_linux_setup_header_t hdr = valid_header();
    hdr.header = 0;
    CHECK_HEX("wrong HdrS magic is rejected", 0, hype_linux_header_is_valid(&hdr));
}

static void test_header_is_valid_rejects_old_version(void) {
    hype_linux_setup_header_t hdr = valid_header();
    hdr.version = 0x0200;
    CHECK_HEX("version below 2.10 is rejected", 0, hype_linux_header_is_valid(&hdr));
}

static void test_header_is_valid_rejects_no_64bit_entry(void) {
    hype_linux_setup_header_t hdr = valid_header();
    hdr.xloadflags = 0;
    CHECK_HEX("missing XLF_KERNEL_64 is rejected (32-bit-only kernel unsupported)", 0,
              hype_linux_header_is_valid(&hdr));
}

static void test_payload_file_offset_zero_means_four(void) {
    hype_linux_setup_header_t hdr = valid_header();
    hdr.setup_sects = 0;
    CHECK_HEX("setup_sects=0 means 4 sectors -> offset (4+1)*512", 5ull * 512ull,
              hype_linux_payload_file_offset(&hdr));
}

static void test_payload_file_offset_explicit_value(void) {
    hype_linux_setup_header_t hdr = valid_header();
    hdr.setup_sects = 30;
    CHECK_HEX("setup_sects=30 -> offset (30+1)*512", 31ull * 512ull,
              hype_linux_payload_file_offset(&hdr));
}

static void test_64bit_entry_is_load_address_plus_0x200(void) {
    CHECK_HEX("entry = load address + 0x200", 0x100200ULL, hype_linux_64bit_entry(0x100000ULL));
}

static void test_build_zero_page_zeroes_first(void) {
    hype_linux_boot_params_t params;
    hype_linux_setup_header_t hdr = valid_header();
    unsigned char *bytes = (unsigned char *)&params;
    unsigned long long i;

    for (i = 0; i < sizeof(params); i++) {
        bytes[i] = 0xAA;
    }

    hype_linux_build_zero_page(&params, &hdr, 0, 0, 0, 0, 0);

    /* A trailing reserved byte this function never touches should end
     * up zeroed, not left as the 0xAA sentinel. */
    CHECK_HEX("trailing reserved region is zeroed, not left dirty", 0,
              params.reserved_0xCD0[sizeof(params.reserved_0xCD0) - 1]);
}

static void test_build_zero_page_overrides_loader_fields(void) {
    hype_linux_boot_params_t params;
    hype_linux_setup_header_t hdr = valid_header();
    hdr.type_of_loader = 0x00; /* whatever the file happened to have */
    hdr.loadflags = 0xFF;

    hype_linux_build_zero_page(&params, &hdr, 0x2000000, 0x300000, 0x20000, 0, 0);

    CHECK_HEX("type_of_loader overridden to UNDEFINED", HYPE_LINUX_TYPE_OF_LOADER_UNDEFINED,
              params.hdr.type_of_loader);
    CHECK_HEX("loadflags overridden to LOADED_HIGH only", HYPE_LINUX_LOADFLAGS_LOADED_HIGH,
              params.hdr.loadflags);
    CHECK_HEX("ramdisk_image set", 0x2000000, params.hdr.ramdisk_image);
    CHECK_HEX("ramdisk_size set", 0x300000, params.hdr.ramdisk_size);
    CHECK_HEX("cmd_line_ptr set", 0x20000, params.hdr.cmd_line_ptr);
    CHECK_HEX("other header fields (e.g. version) copied through unchanged", hdr.version,
              params.hdr.version);
}

static void test_build_zero_page_copies_e820_entries(void) {
    hype_linux_boot_params_t params;
    hype_linux_setup_header_t hdr = valid_header();
    hype_linux_e820_entry_t entries[2];

    entries[0].addr = 0;
    entries[0].size = 0x9FC00;
    entries[0].type = HYPE_LINUX_E820_TYPE_RAM;
    entries[1].addr = 0x100000;
    entries[1].size = 0x1FF00000;
    entries[1].type = HYPE_LINUX_E820_TYPE_RAM;

    hype_linux_build_zero_page(&params, &hdr, 0, 0, 0, entries, 2);

    CHECK_HEX("e820_entries count set", 2, params.e820_entries);
    CHECK_HEX("e820_table[0].addr", 0, params.e820_table[0].addr);
    CHECK_HEX("e820_table[0].size", 0x9FC00, params.e820_table[0].size);
    CHECK_HEX("e820_table[1].addr", 0x100000, params.e820_table[1].addr);
    CHECK_HEX("e820_table[1].size", 0x1FF00000, params.e820_table[1].size);
    CHECK_HEX("e820_table[1].type", HYPE_LINUX_E820_TYPE_RAM, params.e820_table[1].type);
}

static void test_build_zero_page_clamps_e820_count(void) {
    hype_linux_boot_params_t params;
    hype_linux_setup_header_t hdr = valid_header();
    /* Caller's own array must actually hold `count` entries (255) --
     * clamping protects params->e820_table's bound, not a caller that
     * lies about its own input array's size. */
    hype_linux_e820_entry_t entries[255];
    int i;

    for (i = 0; i < 255; i++) {
        entries[i].addr = (uint64_t)i * 0x1000ULL;
        entries[i].size = 0x1000;
        entries[i].type = HYPE_LINUX_E820_TYPE_RAM;
    }

    hype_linux_build_zero_page(&params, &hdr, 0, 0, 0, entries, 255);

    CHECK_HEX("e820_entries clamped to the table's real capacity", HYPE_LINUX_E820_MAX_ENTRIES,
              params.e820_entries);
}

int main(void) {
    test_struct_sizes();
    test_field_offsets();
    test_zero_page_field_offsets();
    test_header_is_valid_accepts_good_header();
    test_header_is_valid_rejects_bad_boot_flag();
    test_header_is_valid_rejects_bad_magic();
    test_header_is_valid_rejects_old_version();
    test_header_is_valid_rejects_no_64bit_entry();
    test_payload_file_offset_zero_means_four();
    test_payload_file_offset_explicit_value();
    test_64bit_entry_is_load_address_plus_0x200();
    test_build_zero_page_zeroes_first();
    test_build_zero_page_overrides_loader_fields();
    test_build_zero_page_copies_e820_entries();
    test_build_zero_page_clamps_e820_count();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
