#include <stdio.h>
#include <string.h>
#include "../../devices/acpi_loader.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

#define CHECK_STR(desc, expected, actual) \
    do { \
        if (strcmp((expected), (actual)) != 0) { \
            printf("FAIL: %s: expected \"%s\", got \"%s\"\n", (desc), (expected), (actual)); \
            failures++; \
        } \
    } while (0)

static void test_set_file_name_pads_and_terminates(void) {
    char dst[HYPE_ACPI_LOADER_FILESZ];
    int rc = hype_acpi_loader_set_file_name(dst, "etc/acpi/rsdp");
    int i;
    int all_zero_after = 1;

    CHECK_HEX("set_file_name succeeds", 0, rc);
    CHECK_STR("name copied correctly", "etc/acpi/rsdp", dst);
    for (i = (int)strlen("etc/acpi/rsdp") + 1; i < HYPE_ACPI_LOADER_FILESZ; i++) {
        if (dst[i] != 0) {
            all_zero_after = 0;
        }
    }
    CHECK_HEX("remainder zero-padded", 1, all_zero_after);
}

static void test_set_file_name_rejects_too_long(void) {
    char dst[HYPE_ACPI_LOADER_FILESZ];
    char too_long[HYPE_ACPI_LOADER_FILESZ + 1];
    int i;

    for (i = 0; i < HYPE_ACPI_LOADER_FILESZ; i++) {
        too_long[i] = 'a';
    }
    too_long[HYPE_ACPI_LOADER_FILESZ] = '\0';

    if (hype_acpi_loader_set_file_name(dst, too_long) == 0) {
        printf("FAIL: name exactly filling the buffer (no room for NUL) should be rejected\n");
        failures++;
    }
}

static void test_build_allocate(void) {
    hype_acpi_loader_entry_t e;
    hype_acpi_loader_build_allocate(&e, "etc/acpi/tables", 64, HYPE_ACPI_LOADER_ZONE_HIGH);

    CHECK_HEX("command", HYPE_ACPI_LOADER_CMD_ALLOCATE, e.command);
    CHECK_STR("file", "etc/acpi/tables", e.alloc.file);
    CHECK_HEX("align", 64, e.alloc.align);
    CHECK_HEX("zone", HYPE_ACPI_LOADER_ZONE_HIGH, e.alloc.zone);
}

static void test_build_add_pointer(void) {
    hype_acpi_loader_entry_t e;
    hype_acpi_loader_build_add_pointer(&e, "etc/acpi/rsdp", "etc/acpi/tables", 24, 8);

    CHECK_HEX("command", HYPE_ACPI_LOADER_CMD_ADD_POINTER, e.command);
    CHECK_STR("dest_file", "etc/acpi/rsdp", e.pointer.dest_file);
    CHECK_STR("src_file", "etc/acpi/tables", e.pointer.src_file);
    CHECK_HEX("offset", 24, e.pointer.offset);
    CHECK_HEX("size", 8, e.pointer.size);
}

static void test_build_add_checksum(void) {
    hype_acpi_loader_entry_t e;
    hype_acpi_loader_build_add_checksum(&e, "etc/acpi/tables", 9, 0, 60);

    CHECK_HEX("command", HYPE_ACPI_LOADER_CMD_ADD_CHECKSUM, e.command);
    CHECK_STR("file", "etc/acpi/tables", e.cksum.file);
    CHECK_HEX("offset", 9, e.cksum.offset);
    CHECK_HEX("start", 0, e.cksum.start);
    CHECK_HEX("length", 60, e.cksum.length);
}

static void test_build_script_entry_count_and_shape(void) {
    hype_acpi_layout_t layout;
    hype_acpi_loader_entry_t entries[HYPE_ACPI_LOADER_SCRIPT_ENTRIES];
    uint32_t n;
    uint32_t allocate_count = 0, pointer_count = 0, checksum_count = 0;
    uint32_t i;

    layout.xsdt_offset = 0;
    layout.xsdt_length = 60;
    layout.fadt_offset = 60;
    layout.fadt_length = 276;
    layout.madt_offset = 336;
    layout.madt_length = 66;
    layout.mcfg_offset = 402;
    layout.mcfg_length = 60;
    layout.dsdt_offset = 462;
    layout.dsdt_length = 36;
    layout.total_length = 498;

    n = hype_acpi_loader_build_script(entries, &layout);
    CHECK_HEX("total entry count", HYPE_ACPI_LOADER_SCRIPT_ENTRIES, n);

    for (i = 0; i < n; i++) {
        if (entries[i].command == HYPE_ACPI_LOADER_CMD_ALLOCATE) {
            allocate_count++;
        } else if (entries[i].command == HYPE_ACPI_LOADER_CMD_ADD_POINTER) {
            pointer_count++;
        } else if (entries[i].command == HYPE_ACPI_LOADER_CMD_ADD_CHECKSUM) {
            checksum_count++;
        } else {
            printf("FAIL: entry %u has an unrecognized command 0x%x\n", i, entries[i].command);
            failures++;
        }
    }
    CHECK_HEX("2 ALLOCATE entries (rsdp + tables)", 2, allocate_count);
    CHECK_HEX("6 ADD_POINTER entries (rsdp->xsdt, xsdt->{fadt,madt,mcfg}, fadt->{dsdt,x_dsdt})", 6,
              pointer_count);
    CHECK_HEX("7 ADD_CHECKSUM entries (5 tables + rsdp's own 2)", 7, checksum_count);

    /* Spot-check a couple of the pointer entries land on the offsets
     * this specific layout implies. */
    CHECK_STR("first ALLOCATE targets rsdp", "etc/acpi/rsdp", entries[0].alloc.file);
    CHECK_STR("second ALLOCATE targets tables", "etc/acpi/tables", entries[1].alloc.file);
    CHECK_HEX("rsdp->xsdt pointer offset (RSDP.xsdt_address)", 24, entries[2].pointer.offset);
    CHECK_HEX("xsdt->fadt pointer offset", layout.xsdt_offset + 36, entries[3].pointer.offset);
    CHECK_HEX("xsdt->madt pointer offset", layout.xsdt_offset + 36 + 8, entries[4].pointer.offset);
    CHECK_HEX("xsdt->mcfg pointer offset", layout.xsdt_offset + 36 + 16, entries[5].pointer.offset);
    CHECK_HEX("fadt->dsdt (legacy 32-bit) pointer offset", layout.fadt_offset + 40, entries[6].pointer.offset);
    CHECK_HEX("fadt->dsdt (legacy) pointer size", 4, entries[6].pointer.size);
    CHECK_HEX("fadt->x_dsdt pointer offset", layout.fadt_offset + 140, entries[7].pointer.offset);
    CHECK_HEX("fadt->x_dsdt pointer size", 8, entries[7].pointer.size);
}

int main(void) {
    test_set_file_name_pads_and_terminates();
    test_set_file_name_rejects_too_long();
    test_build_allocate();
    test_build_add_pointer();
    test_build_add_checksum();
    test_build_script_entry_count_and_shape();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
