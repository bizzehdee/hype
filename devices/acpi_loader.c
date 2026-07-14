#include "acpi_loader.h"

#include <stddef.h>

static void zero_entry(hype_acpi_loader_entry_t *e) {
    uint8_t *bytes = (uint8_t *)e;
    uint32_t i;
    for (i = 0; i < sizeof(*e); i++) {
        bytes[i] = 0;
    }
}

int hype_acpi_loader_set_file_name(char dst[HYPE_ACPI_LOADER_FILESZ], const char *name) {
    int i = 0;

    while (name[i] != '\0') {
        if (i >= HYPE_ACPI_LOADER_FILESZ - 1) {
            return -1;
        }
        dst[i] = name[i];
        i++;
    }
    for (; i < HYPE_ACPI_LOADER_FILESZ; i++) {
        dst[i] = '\0';
    }
    return 0;
}

void hype_acpi_loader_build_allocate(hype_acpi_loader_entry_t *e, const char *file, uint32_t align,
                                      uint8_t zone) {
    zero_entry(e);
    e->command = HYPE_ACPI_LOADER_CMD_ALLOCATE;
    hype_acpi_loader_set_file_name(e->alloc.file, file);
    e->alloc.align = align;
    e->alloc.zone = zone;
}

void hype_acpi_loader_build_add_pointer(hype_acpi_loader_entry_t *e, const char *dest_file,
                                        const char *src_file, uint32_t offset, uint8_t size) {
    zero_entry(e);
    e->command = HYPE_ACPI_LOADER_CMD_ADD_POINTER;
    hype_acpi_loader_set_file_name(e->pointer.dest_file, dest_file);
    hype_acpi_loader_set_file_name(e->pointer.src_file, src_file);
    e->pointer.offset = offset;
    e->pointer.size = size;
}

void hype_acpi_loader_build_add_checksum(hype_acpi_loader_entry_t *e, const char *file, uint32_t offset,
                                          uint32_t start, uint32_t length) {
    zero_entry(e);
    e->command = HYPE_ACPI_LOADER_CMD_ADD_CHECKSUM;
    hype_acpi_loader_set_file_name(e->cksum.file, file);
    e->cksum.offset = offset;
    e->cksum.start = start;
    e->cksum.length = length;
}

uint32_t hype_acpi_loader_build_script(hype_acpi_loader_entry_t *entries, const hype_acpi_layout_t *layout) {
    uint32_t n = 0;

    hype_acpi_loader_build_allocate(&entries[n++], HYPE_ACPI_LOADER_FILE_RSDP, 16,
                                     HYPE_ACPI_LOADER_ZONE_FSEG);
    hype_acpi_loader_build_allocate(&entries[n++], HYPE_ACPI_LOADER_FILE_TABLES, 64,
                                     HYPE_ACPI_LOADER_ZONE_HIGH);

    /* RSDP.XsdtAddress -> XSDT (offset 0 within "etc/acpi/tables", by
     * construction -- hype_acpi_build_tables_blob() always places XSDT
     * first). */
    hype_acpi_loader_build_add_pointer(&entries[n++], HYPE_ACPI_LOADER_FILE_RSDP,
                                        HYPE_ACPI_LOADER_FILE_TABLES,
                                        (uint32_t)offsetof(hype_acpi_rsdp_t, xsdt_address), 8);

    /* XSDT's 3 entries -> FADT/MADT/MCFG, all within the same blob. */
    hype_acpi_loader_build_add_pointer(
        &entries[n++], HYPE_ACPI_LOADER_FILE_TABLES, HYPE_ACPI_LOADER_FILE_TABLES,
        layout->xsdt_offset + (uint32_t)sizeof(hype_acpi_sdt_header_t) + 0u * 8u, 8);
    hype_acpi_loader_build_add_pointer(
        &entries[n++], HYPE_ACPI_LOADER_FILE_TABLES, HYPE_ACPI_LOADER_FILE_TABLES,
        layout->xsdt_offset + (uint32_t)sizeof(hype_acpi_sdt_header_t) + 1u * 8u, 8);
    hype_acpi_loader_build_add_pointer(
        &entries[n++], HYPE_ACPI_LOADER_FILE_TABLES, HYPE_ACPI_LOADER_FILE_TABLES,
        layout->xsdt_offset + (uint32_t)sizeof(hype_acpi_sdt_header_t) + 2u * 8u, 8);

    /* FADT.Dsdt (legacy 32-bit) and FADT.X_Dsdt (64-bit) -> DSDT,
     * within the same blob. */
    hype_acpi_loader_build_add_pointer(&entries[n++], HYPE_ACPI_LOADER_FILE_TABLES,
                                        HYPE_ACPI_LOADER_FILE_TABLES,
                                        layout->fadt_offset + (uint32_t)offsetof(hype_acpi_fadt_t, dsdt), 4);
    hype_acpi_loader_build_add_pointer(&entries[n++], HYPE_ACPI_LOADER_FILE_TABLES,
                                        HYPE_ACPI_LOADER_FILE_TABLES,
                                        layout->fadt_offset + (uint32_t)offsetof(hype_acpi_fadt_t, x_dsdt), 8);

    /* One checksum per table in "etc/acpi/tables" -- the Checksum byte
     * is always at offset 9 within any table's own ACPI SDT header
     * (offsetof(hype_acpi_sdt_header_t, checksum)). */
    hype_acpi_loader_build_add_checksum(
        &entries[n++], HYPE_ACPI_LOADER_FILE_TABLES,
        layout->xsdt_offset + (uint32_t)offsetof(hype_acpi_sdt_header_t, checksum), layout->xsdt_offset,
        layout->xsdt_length);
    hype_acpi_loader_build_add_checksum(
        &entries[n++], HYPE_ACPI_LOADER_FILE_TABLES,
        layout->fadt_offset + (uint32_t)offsetof(hype_acpi_sdt_header_t, checksum), layout->fadt_offset,
        layout->fadt_length);
    hype_acpi_loader_build_add_checksum(
        &entries[n++], HYPE_ACPI_LOADER_FILE_TABLES,
        layout->madt_offset + (uint32_t)offsetof(hype_acpi_sdt_header_t, checksum), layout->madt_offset,
        layout->madt_length);
    hype_acpi_loader_build_add_checksum(
        &entries[n++], HYPE_ACPI_LOADER_FILE_TABLES,
        layout->mcfg_offset + (uint32_t)offsetof(hype_acpi_sdt_header_t, checksum), layout->mcfg_offset,
        layout->mcfg_length);
    hype_acpi_loader_build_add_checksum(
        &entries[n++], HYPE_ACPI_LOADER_FILE_TABLES,
        layout->dsdt_offset + (uint32_t)offsetof(hype_acpi_sdt_header_t, checksum), layout->dsdt_offset,
        layout->dsdt_length);

    /* RSDP's own two checksums: the ACPI 1.0 region [0,20) and the
     * whole 36-byte extended structure. */
    hype_acpi_loader_build_add_checksum(&entries[n++], HYPE_ACPI_LOADER_FILE_RSDP,
                                        (uint32_t)offsetof(hype_acpi_rsdp_t, checksum), 0, 20);
    hype_acpi_loader_build_add_checksum(&entries[n++], HYPE_ACPI_LOADER_FILE_RSDP,
                                        (uint32_t)offsetof(hype_acpi_rsdp_t, extended_checksum), 0, 36);

    return n;
}
