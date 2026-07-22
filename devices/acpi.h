#ifndef HYPE_DEVICES_ACPI_H
#define HYPE_DEVICES_ACPI_H

#include <stdint.h>

/*
 * Per-VM ACPI table synthesis (M4-4). Field layouts below are
 * transcribed directly from ACPICA's own reference headers (actbl.h/
 * actbl1.h/actbl2.h -- the canonical, most-implementations-agree-with-it
 * source for these structures), not reconstructed from memory, same
 * discipline as arch/x86_64/svm/vmcb.h's own layout comment -- a wrong
 * offset here is a wire-format bug, not a logic bug.
 *
 * These builders produce table CONTENT only, laid out as a single
 * relocatable blob with every cross-table pointer field pre-filled
 * with the TARGET's byte offset *within that same blob* (not a final
 * address) and every table's own Checksum byte left at 0 -- because
 * the guest firmware (real, vendored OVMF, M4-2) does not consume
 * ACPI content built ahead of time at some fixed physical address the
 * way this project's own earlier hand-written test guests received
 * their payloads. Stock OVMF's AcpiPlatformDxe driver only ever
 * fetches ACPI content via QEMU's fw_cfg device (devices/fw_cfg.h)
 * and relocates/patches/checksums it itself, guided by a separate
 * "linker/loader" script (devices/acpi_loader.h) that names exactly
 * which offsets need a pointer added or a checksum computed, once the
 * guest has allocated real memory for this blob. This is why these
 * builders never compute a final absolute address or a final
 * checksum themselves -- both are guest-side operations by design, and
 * precomputing either here would just be silently wrong the moment the
 * guest firmware allocates the blob somewhere other than where we
 * guessed.
 */

/* ACPI_TABLE_HEADER -- common 36-byte header prefixing every ACPI
 * table (RSDT/XSDT/FADT/MADT/MCFG/DSDT/...). */
typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    char creator_id[4];
    uint32_t creator_revision;
} __attribute__((packed)) hype_acpi_sdt_header_t;

_Static_assert(sizeof(hype_acpi_sdt_header_t) == 36, "ACPI SDT header must be 36 bytes");

/* RSDP (ACPI 2.0+ 36-byte form -- includes the XSDT pointer and the
 * extended 36-byte checksum; the legacy 32-bit RsdtAddress is left 0,
 * matching a 64-bit-guest-only target, plan.md §10 decision #23). */
typedef struct {
    char signature[8]; /* "RSD PTR " (note the trailing space) */
    uint8_t checksum;  /* covers bytes [0,20) only -- the ACPI 1.0 region */
    char oem_id[6];
    uint8_t revision; /* 2 */
    uint32_t rsdt_address; /* deprecated, left 0 */
    uint32_t length;       /* 36 */
    uint64_t xsdt_address;
    uint8_t extended_checksum; /* covers the whole 36-byte structure */
    uint8_t reserved[3];
} __attribute__((packed)) hype_acpi_rsdp_t;

_Static_assert(sizeof(hype_acpi_rsdp_t) == 36, "RSDP must be 36 bytes");

/* ACPI_GENERIC_ADDRESS -- 12 bytes. */
typedef struct {
    uint8_t space_id;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_width;
    uint64_t address;
} __attribute__((packed)) hype_acpi_gas_t;

_Static_assert(sizeof(hype_acpi_gas_t) == 12, "Generic Address Structure must be 12 bytes");

#define HYPE_ACPI_GAS_SPACE_SYSTEM_MEMORY 0u
#define HYPE_ACPI_GAS_SPACE_SYSTEM_IO 1u

/* FADT ("FACP"), ACPI 6.x layout -- 276 bytes total (header + every
 * field below sums to exactly 276, cross-checked against the
 * well-known real-world FADT size as a sanity check on the field
 * transcription). This project targets ACPI's "Hardware Reduced"
 * profile (HYPE_ACPI_FADT_HW_REDUCED below) -- the legacy PM1a/PM2/
 * PM-timer/GPE fixed-hardware blocks are all left zeroed/unused, and
 * S5 (power-off) goes through SleepControl/SleepStatus instead of the
 * classic PM1a_CNT SLP_TYP/SLP_EN sequence. Wiring SleepControl to a
 * real device model (so a guest OS can actually power itself off) is
 * explicitly deferred -- plan.md's host power-lifecycle milestone
 * (M9) is where the GPE-based graceful-shutdown story gets built;
 * this milestone only needs the table itself to be well-formed. */
typedef struct {
    hype_acpi_sdt_header_t header;
    uint32_t facs;
    uint32_t dsdt;
    uint8_t model;
    uint8_t preferred_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4_bios_request;
    uint8_t pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;
    uint32_t pm1b_control_block;
    uint32_t pm2_control_block;
    uint32_t pm_timer_block;
    uint32_t gpe0_block;
    uint32_t gpe1_block;
    uint8_t pm1_event_length;
    uint8_t pm1_control_length;
    uint8_t pm2_control_length;
    uint8_t pm_timer_length;
    uint8_t gpe0_block_length;
    uint8_t gpe1_block_length;
    uint8_t gpe1_base;
    uint8_t cst_control;
    uint16_t c2_latency;
    uint16_t c3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alarm;
    uint8_t month_alarm;
    uint8_t century;
    uint16_t boot_flags;
    uint8_t reserved;
    uint32_t flags;
    hype_acpi_gas_t reset_register;
    uint8_t reset_value;
    uint16_t arm_boot_flags;
    uint8_t minor_revision;
    uint64_t x_facs;
    uint64_t x_dsdt;
    hype_acpi_gas_t x_pm1a_event_block;
    hype_acpi_gas_t x_pm1b_event_block;
    hype_acpi_gas_t x_pm1a_control_block;
    hype_acpi_gas_t x_pm1b_control_block;
    hype_acpi_gas_t x_pm2_control_block;
    hype_acpi_gas_t x_pm_timer_block;
    hype_acpi_gas_t x_gpe0_block;
    hype_acpi_gas_t x_gpe1_block;
    hype_acpi_gas_t sleep_control;
    hype_acpi_gas_t sleep_status;
    uint64_t hypervisor_id;
} __attribute__((packed)) hype_acpi_fadt_t;

_Static_assert(sizeof(hype_acpi_fadt_t) == 276, "FADT must be 276 bytes (ACPI 6.x layout)");

/* FADT Flags bits this project actually sets. */
#define HYPE_ACPI_FADT_WBINVD (1u << 0)        /* WBINVD correctly flushes all caches -- true on real x86 */
#define HYPE_ACPI_FADT_PWR_BUTTON (1u << 4)    /* no fixed-hardware power button (none emulated yet) */
#define HYPE_ACPI_FADT_SLP_BUTTON (1u << 5)    /* no fixed-hardware sleep button */
#define HYPE_ACPI_FADT_HW_REDUCED_ACPI (1u << 20)

/* M8-6: the QEMU/OVMF ACPI PM1a control register (classic ACPI, I/O 0x604) that
 * a UEFI guest's OS ends up writing (via EFI ResetSystem -> OVMF) to power off.
 * A write with PM1_CNT.SLP_EN (bit 13) set commits the sleep; with only \_S5
 * declared, that is an orderly S5 power-off, which hype detects in the IOIO path
 * (boot/main.c) and turns into an S5 lifecycle event. */
#define HYPE_ACPI_PM1A_CNT_PORT 0x604u
#define HYPE_ACPI_PM1_SLP_EN (1u << 13) /* PM1_CNT bit 13: commit the sleep */

/* MADT ("APIC") header -- common SDT header plus the two MADT-specific
 * fields, before the variable-length list of subtables. */
typedef struct {
    hype_acpi_sdt_header_t header;
    uint32_t local_apic_address;
    uint32_t flags; /* bit0 = PCAT_COMPAT (dual 8259 PICs also present) */
} __attribute__((packed)) hype_acpi_madt_header_t;

_Static_assert(sizeof(hype_acpi_madt_header_t) == 44, "MADT header must be 44 bytes");

#define HYPE_ACPI_MADT_PCAT_COMPAT (1u << 0)

/* ACPI_SUBTABLE_HEADER -- common 2-byte prefix for every MADT entry. */
typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) hype_acpi_madt_subtable_header_t;

_Static_assert(sizeof(hype_acpi_madt_subtable_header_t) == 2, "MADT subtable header must be 2 bytes");

#define HYPE_ACPI_MADT_TYPE_LOCAL_APIC 0u
#define HYPE_ACPI_MADT_TYPE_IO_APIC 1u
#define HYPE_ACPI_MADT_TYPE_INTERRUPT_OVERRIDE 2u

#define HYPE_ACPI_MADT_LOCAL_APIC_ENABLED (1u << 0)

typedef struct {
    hype_acpi_madt_subtable_header_t header; /* type=0, length=8 */
    uint8_t processor_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed)) hype_acpi_madt_local_apic_t;

_Static_assert(sizeof(hype_acpi_madt_local_apic_t) == 8, "MADT local APIC entry must be 8 bytes");

typedef struct {
    hype_acpi_madt_subtable_header_t header; /* type=1, length=12 */
    uint8_t io_apic_id;
    uint8_t reserved;
    uint32_t io_apic_address;
    uint32_t global_irq_base;
} __attribute__((packed)) hype_acpi_madt_io_apic_t;

_Static_assert(sizeof(hype_acpi_madt_io_apic_t) == 12, "MADT I/O APIC entry must be 12 bytes");

typedef struct {
    hype_acpi_madt_subtable_header_t header; /* type=2, length=10 */
    uint8_t bus;
    uint8_t source_irq;
    uint32_t global_irq;
    uint16_t flags;
} __attribute__((packed)) hype_acpi_madt_interrupt_override_t;

_Static_assert(sizeof(hype_acpi_madt_interrupt_override_t) == 10,
               "MADT interrupt source override entry must be 10 bytes");

/* MCFG ("MCFG") header -- common SDT header plus 8 reserved bytes,
 * before the variable-length list of allocation entries. */
typedef struct {
    hype_acpi_sdt_header_t header;
    uint8_t reserved[8];
} __attribute__((packed)) hype_acpi_mcfg_header_t;

_Static_assert(sizeof(hype_acpi_mcfg_header_t) == 44, "MCFG header must be 44 bytes");

typedef struct {
    uint64_t base_address;
    uint16_t pci_segment;
    uint8_t start_bus;
    uint8_t end_bus;
    uint32_t reserved;
} __attribute__((packed)) hype_acpi_mcfg_allocation_t;

_Static_assert(sizeof(hype_acpi_mcfg_allocation_t) == 16, "MCFG allocation entry must be 16 bytes");

/* Generous ceiling matching this project's other fixed-size buffers
 * (e.g. arch/x86_64/svm/npt.h's HYPE_NPT_MAX_GB) -- revisit once real
 * per-VM vCPU-count sizing exists. */
#define HYPE_ACPI_MAX_CPUS 16

typedef struct {
    uint8_t apic_ids[HYPE_ACPI_MAX_CPUS];
    uint8_t cpu_count;
    uint32_t local_apic_address; /* e.g. 0xFEE00000, the standard x86 LAPIC MMIO base */
    uint8_t io_apic_id;
    uint32_t io_apic_address; /* e.g. 0xFEC00000, the standard x86 IOAPIC MMIO base */
    uint32_t io_apic_gsi_base;
    /* PCIe ECAM base for MCFG -- not yet backed by a real MMIO/PCI
     * config-space device model (a future PCI milestone's job); this
     * table entry is built now so guest OSes can at least discover a
     * well-formed MCFG, matching this project's established "build the
     * reusable primitive now, wire the harder integration later"
     * pattern (e.g. M4-3's flash persistence). */
    uint64_t mcfg_base_address;
    uint16_t pci_segment;
    uint8_t pci_start_bus;
    uint8_t pci_end_bus;
    uint32_t sci_interrupt;
} hype_acpi_config_t;

/* Byte offsets/lengths of each table within the blob
 * hype_acpi_build_tables_blob() fills -- both a discoverability report
 * for the caller and, more importantly, exactly the information
 * devices/acpi_loader.h's builder needs to emit ADD_POINTER/
 * ADD_CHECKSUM commands against the right byte ranges. */
typedef struct {
    uint32_t xsdt_offset;
    uint32_t xsdt_length;
    uint32_t fadt_offset;
    uint32_t fadt_length;
    uint32_t madt_offset;
    uint32_t madt_length;
    uint32_t mcfg_offset;
    uint32_t mcfg_length;
    uint32_t dsdt_offset;
    uint32_t dsdt_length;
    uint32_t total_length;
} hype_acpi_layout_t;

/*
 * Computes -X for each byte X over bytes[0,length), matching the ACPI
 * checksum rule (the full range, including the checksum byte itself,
 * must sum to 0 mod 256) -- callers needing a final, self-contained
 * checksum (nothing else patches the table afterward) pass a buffer
 * with the checksum byte already 0; the returned value is what to
 * write there. Pure computation, no I/O.
 */
uint8_t hype_acpi_checksum(const uint8_t *bytes, uint32_t length);

/*
 * Fills `rsdp` (ACPI 2.0+ form): signature/OEM ID/revision=2/length=36.
 * `xsdt_offset_in_tables_blob` is stored directly into xsdt_address --
 * NOT a final address, but the byte offset of XSDT within the separate
 * "etc/acpi/tables" blob (devices/acpi_loader.h's ADD_POINTER command
 * adds the blob's real allocated base to this value once the guest
 * firmware allocates it). Both checksum bytes are left 0 -- also
 * guest-patched, via ADD_CHECKSUM, after the pointer is applied. Pure
 * struct-filling.
 */
void hype_acpi_build_rsdp(hype_acpi_rsdp_t *rsdp, uint64_t xsdt_offset_in_tables_blob);

/*
 * Builds XSDT+FADT+MADT+MCFG+DSDT into `buf` (of `buf_size` bytes,
 * zeroed first), reporting each table's offset/length in `*out`.
 * Every cross-table pointer field (XSDT's 3 entries; FADT's Dsdt/
 * X_Dsdt) is pre-filled with the *target's offset within this same
 * buffer*, not a final address -- see this header's own top comment.
 * Every table's Checksum byte is left 0. Returns 0 on success, -1 if
 * `buf_size` is too small for the content `cfg` describes (e.g. more
 * CPUs than fit) -- the caller's job to size `buf` generously enough
 * up front. Pure struct/buffer filling -- no CPU/guest-memory access,
 * no I/O.
 */
int hype_acpi_build_tables_blob(uint8_t *buf, uint32_t buf_size, const hype_acpi_config_t *cfg,
                                 hype_acpi_layout_t *out);

#endif /* HYPE_DEVICES_ACPI_H */
