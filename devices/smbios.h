#ifndef HYPE_DEVICES_SMBIOS_H
#define HYPE_DEVICES_SMBIOS_H

#include <stdint.h>

/*
 * #436: SMBIOS. hype published none at all -- measured by running the UEFI
 * shell's `smbiosview` as a hype guest, which answered "Cannot get SMBIOS
 * Table", while the same shell under QEMU/KVM reports a full set. Every real
 * x86 platform provides SMBIOS, firmware installs it as a UEFI configuration
 * table, and an OS reads it during early initialisation, so a machine without
 * one is describing itself as something no physical system is.
 *
 * The structures are handed to firmware the way every other table here is: as
 * fw_cfg files ("etc/smbios/smbios-anchor" and "etc/smbios/smbios-tables"),
 * which OVMF reads and republishes.
 *
 * Only structures whose contents hype can answer truthfully are emitted. Type
 * 4 reports the one processor hype gives a guest, type 16/17 report the RAM it
 * was actually given, and nothing here claims a capability the machine lacks --
 * the same rule the ACPI tables in this directory follow.
 */

/* Structure types emitted. */
#define HYPE_SMBIOS_TYPE_BIOS 0u
#define HYPE_SMBIOS_TYPE_SYSTEM 1u
#define HYPE_SMBIOS_TYPE_BASEBOARD 2u
#define HYPE_SMBIOS_TYPE_CHASSIS 3u
#define HYPE_SMBIOS_TYPE_PROCESSOR 4u
#define HYPE_SMBIOS_TYPE_MEMORY_ARRAY 16u
#define HYPE_SMBIOS_TYPE_MEMORY_DEVICE 17u
#define HYPE_SMBIOS_TYPE_END 127u

/* Every structure starts with this. `handle` is unique per structure. */
typedef struct {
    uint8_t type;
    uint8_t length; /* formatted area only, excluding the string set */
    uint16_t handle;
} __attribute__((packed)) hype_smbios_header_t;

/* SMBIOS 3.0 entry point ("_SM3_"), 24 bytes. */
typedef struct {
    uint8_t anchor[5];
    uint8_t checksum;
    uint8_t length;
    uint8_t major_version;
    uint8_t minor_version;
    uint8_t docrev;
    uint8_t revision;
    uint8_t reserved;
    uint32_t table_max_size;
    uint64_t table_address;
} __attribute__((packed)) hype_smbios_entry_point_t;

typedef struct {
    uint32_t anchor_length; /* bytes written to the anchor buffer */
    uint32_t tables_length; /* bytes written to the tables buffer */
} hype_smbios_layout_t;

typedef struct {
    /*
     * Guest-visible LOGICAL CPUs -- the same number CPUID leaf 0xB/0x1F is told, so the two
     * guest-visible topology surfaces cannot disagree.
     */
    uint32_t cpu_count;
    /*
     * #562: SMT threads per core this VM was granted, from the placement that also feeds
     * vmm_set_topology(). 0 is read as 1 (no SMT).
     *
     * The CORE count is DERIVED from these two rather than passed in. That is deliberate: a third
     * field could disagree with the first two, and computing one guest's topology twice from two
     * places is exactly what #559, #560 and #561 each were.
     */
    uint32_t threads_per_core;
    uint64_t ram_bytes;
} hype_smbios_config_t;

/*
 * Fill `anchor` (>= sizeof(hype_smbios_entry_point_t)) and `tables`
 * (>= HYPE_SMBIOS_TABLES_MIN_SIZE) with the structure set. Returns 0 on
 * success, -1 if a buffer is too small or the configuration is not sane.
 *
 * The anchor's table_address is left at 0: firmware relocates the structures
 * and patches it, exactly as the ACPI table-loader patches table pointers.
 */
#define HYPE_SMBIOS_TABLES_MIN_SIZE 512u

int hype_smbios_build(const hype_smbios_config_t *cfg, uint8_t *anchor, uint32_t anchor_size,
                      uint8_t *tables, uint32_t tables_size, hype_smbios_layout_t *out);

#endif /* HYPE_DEVICES_SMBIOS_H */
