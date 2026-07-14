#ifndef HYPE_DEVICES_ACPI_LOADER_H
#define HYPE_DEVICES_ACPI_LOADER_H

#include <stdint.h>

#include "acpi.h"

/*
 * QEMU "BIOS linker/loader" script builder (M4-4) -- the wire format
 * served as the "etc/table-loader" fw_cfg file (devices/fw_cfg.h).
 * Stock, unmodified OVMF (this project's own vendored guest firmware,
 * M4-2) already knows how to interpret this exact format; it is how
 * real QEMU tells guest firmware to allocate memory for the ACPI
 * blobs it fetched via fw_cfg, patch cross-table pointers once real
 * addresses are known, and (re)compute each table's checksum after
 * patching -- see devices/acpi.h's own top comment for why table
 * content is built with relative offsets instead of final addresses
 * in the first place. Struct layout and command/zone values below are
 * transcribed directly from QEMU's own
 * include/hw/acpi/bios-linker-loader.h (BiosLinkerLoaderEntry) --
 * fetched and read for this task, not reconstructed from memory, same
 * discipline as this project's other wire-format structs.
 */

#define HYPE_ACPI_LOADER_FILESZ 56 /* FW_CFG_MAX_FILE_PATH */

#define HYPE_ACPI_LOADER_CMD_ALLOCATE 1u
#define HYPE_ACPI_LOADER_CMD_ADD_POINTER 2u
#define HYPE_ACPI_LOADER_CMD_ADD_CHECKSUM 3u

#define HYPE_ACPI_LOADER_ZONE_HIGH 1u
#define HYPE_ACPI_LOADER_ZONE_FSEG 2u

typedef struct {
    uint32_t command;
    union {
        struct {
            char file[HYPE_ACPI_LOADER_FILESZ];
            uint32_t align;
            uint8_t zone;
        } alloc;
        struct {
            char dest_file[HYPE_ACPI_LOADER_FILESZ];
            char src_file[HYPE_ACPI_LOADER_FILESZ];
            uint32_t offset;
            uint8_t size;
        } pointer;
        struct {
            char file[HYPE_ACPI_LOADER_FILESZ];
            uint32_t offset;
            uint32_t start;
            uint32_t length;
        } cksum;
        uint8_t pad[124];
    };
} __attribute__((packed)) hype_acpi_loader_entry_t;

_Static_assert(sizeof(hype_acpi_loader_entry_t) == 128, "loader entry must be 128 bytes");

/* This project's fixed two-file ACPI delivery: a small "etc/acpi/rsdp"
 * plus one "etc/acpi/tables" blob holding XSDT+FADT+MADT+MCFG+DSDT
 * (devices/acpi.h). */
#define HYPE_ACPI_LOADER_FILE_RSDP "etc/acpi/rsdp"
#define HYPE_ACPI_LOADER_FILE_TABLES "etc/acpi/tables"

/* 2 ALLOCATE + 4 ADD_POINTER (RSDP->XSDT, XSDT->FADT, XSDT->MADT,
 * XSDT->MCFG) + 2 ADD_POINTER (FADT->DSDT, legacy 32-bit + X_Dsdt
 * 64-bit) + 5 ADD_CHECKSUM (one per table in "etc/acpi/tables") + 2
 * ADD_CHECKSUM (RSDP's own two checksums) = 15, fixed for this
 * project's fixed table set. */
#define HYPE_ACPI_LOADER_SCRIPT_ENTRIES 15

/*
 * Copies `name` (a NUL-terminated ASCII fw_cfg file name, must fit
 * within HYPE_ACPI_LOADER_FILESZ - 1 bytes) into `dst`, NUL-terminating
 * and zero-padding the remainder. Returns 0 on success, -1 if `name`
 * is too long to fit. Pure buffer-filling.
 */
int hype_acpi_loader_set_file_name(char dst[HYPE_ACPI_LOADER_FILESZ], const char *name);

/* Fills `e` as an ALLOCATE command. Pure struct-filling. */
void hype_acpi_loader_build_allocate(hype_acpi_loader_entry_t *e, const char *file, uint32_t align,
                                      uint8_t zone);

/* Fills `e` as an ADD_POINTER command. Pure struct-filling. */
void hype_acpi_loader_build_add_pointer(hype_acpi_loader_entry_t *e, const char *dest_file,
                                        const char *src_file, uint32_t offset, uint8_t size);

/* Fills `e` as an ADD_CHECKSUM command. Pure struct-filling. */
void hype_acpi_loader_build_add_checksum(hype_acpi_loader_entry_t *e, const char *file, uint32_t offset,
                                          uint32_t start, uint32_t length);

/*
 * Fills `entries` (must hold at least HYPE_ACPI_LOADER_SCRIPT_ENTRIES
 * elements) with the complete linker/loader script for this project's
 * fixed RSDP + tables-blob ACPI setup, given the layout
 * hype_acpi_build_tables_blob() reported for the "etc/acpi/tables"
 * blob. Returns HYPE_ACPI_LOADER_SCRIPT_ENTRIES. Pure struct-filling,
 * no CPU/guest-memory access, no I/O.
 */
uint32_t hype_acpi_loader_build_script(hype_acpi_loader_entry_t *entries, const hype_acpi_layout_t *layout);

#endif /* HYPE_DEVICES_ACPI_LOADER_H */
