#include "acpi.h"
#include "cmos.h"
#include "dsdt_aml.h" /* M4-6b2: compiled DSDT AML body (PCI host bridge + _PRT) */

static const char HYPE_ACPI_OEM_ID[6] = {'H', 'Y', 'P', 'E', ' ', ' '};
static const char HYPE_ACPI_CREATOR_ID[4] = {'H', 'Y', 'P', 'E'};

/* Writes `value` little-endian into dst[0..7] -- avoids relying on
 * natural 8-byte alignment of `dst` (unlike a field access through a
 * `__attribute__((packed))` struct, a raw `uint64_t *` cast does NOT
 * get unaligned-safe codegen from the compiler, and this project's
 * ACPI table blob packs tables back-to-back at byte granularity, so
 * an 8-byte pointer-array entry is not guaranteed 8-byte aligned).
 * Same technique as boot/main.c's own hype_write_le64(). */
static void write_le64(uint8_t *dst, uint64_t value) {
    int i;
    for (i = 0; i < 8; i++) {
        dst[i] = (uint8_t)(value >> (8 * i));
    }
}

static void fill_header(hype_acpi_sdt_header_t *hdr, const char signature[4], uint32_t length,
                         uint8_t revision, const char oem_table_id[8]) {
    int i;

    for (i = 0; i < 4; i++) {
        hdr->signature[i] = signature[i];
    }
    hdr->length = length;
    hdr->revision = revision;
    hdr->checksum = 0; /* patched by the guest firmware via ADD_CHECKSUM, see acpi.h's top comment */
    for (i = 0; i < 6; i++) {
        hdr->oem_id[i] = HYPE_ACPI_OEM_ID[i];
    }
    for (i = 0; i < 8; i++) {
        hdr->oem_table_id[i] = oem_table_id[i];
    }
    hdr->oem_revision = 1;
    for (i = 0; i < 4; i++) {
        hdr->creator_id[i] = HYPE_ACPI_CREATOR_ID[i];
    }
    hdr->creator_revision = 1;
}

uint8_t hype_acpi_checksum(const uint8_t *bytes, uint32_t length) {
    uint8_t sum = 0;
    uint32_t i;

    for (i = 0; i < length; i++) {
        sum = (uint8_t)(sum + bytes[i]);
    }
    return (uint8_t)(0u - sum);
}

void hype_acpi_build_rsdp(hype_acpi_rsdp_t *rsdp, uint64_t xsdt_offset_in_tables_blob) {
    static const char signature[8] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};
    int i;

    for (i = 0; i < 8; i++) {
        rsdp->signature[i] = signature[i];
    }
    rsdp->checksum = 0; /* patched by the guest firmware via ADD_CHECKSUM */
    for (i = 0; i < 6; i++) {
        rsdp->oem_id[i] = HYPE_ACPI_OEM_ID[i];
    }
    rsdp->revision = 2;
    rsdp->rsdt_address = 0; /* deprecated field, unused -- 64-bit-guest-only target (plan.md §10 #23) */
    rsdp->length = sizeof(*rsdp);
    rsdp->xsdt_address = xsdt_offset_in_tables_blob; /* patched to a real address via ADD_POINTER */
    rsdp->extended_checksum = 0;
    for (i = 0; i < 3; i++) {
        rsdp->reserved[i] = 0;
    }
}

/* #355: little-endian dword into the DSDT body copy. AML stores address-space descriptor fields
 * little-endian regardless of host order, so this is explicit rather than a cast. */
static void put_le32_at(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

int hype_acpi_build_tables_blob(uint8_t *buf, uint32_t buf_size, const hype_acpi_config_t *cfg,
                                 hype_acpi_layout_t *out) {
    uint32_t xsdt_entry_count = 3; /* FADT, MADT, MCFG */
    uint32_t xsdt_length = (uint32_t)sizeof(hype_acpi_sdt_header_t) + xsdt_entry_count * 8u;
    uint32_t fadt_length = (uint32_t)sizeof(hype_acpi_fadt_t);
    uint32_t madt_length = (uint32_t)sizeof(hype_acpi_madt_header_t) +
                            (uint32_t)cfg->cpu_count * (uint32_t)sizeof(hype_acpi_madt_local_apic_t) +
                            (uint32_t)sizeof(hype_acpi_madt_io_apic_t) +
                            (uint32_t)sizeof(hype_acpi_madt_interrupt_override_t);
    uint32_t mcfg_length =
        (uint32_t)sizeof(hype_acpi_mcfg_header_t) + (uint32_t)sizeof(hype_acpi_mcfg_allocation_t);
    uint32_t dsdt_length = (uint32_t)sizeof(hype_acpi_sdt_header_t) + HYPE_DSDT_AML_BODY_LEN;
    uint32_t total = xsdt_length + fadt_length + madt_length + mcfg_length + dsdt_length;
    uint32_t i;

    if (cfg->cpu_count == 0 || cfg->cpu_count > HYPE_ACPI_MAX_CPUS) {
        return -1;
    }
    /* #355: a window that starts inside guest RAM, is unaligned, or has nothing left below the
     * I/O APIC describes memory that is not the bridge's. Refuse rather than emit it. */
    if (cfg->pci_window_base == 0u || (cfg->pci_window_base & 0xFFFFFu) != 0u ||
        cfg->pci_window_base >= (uint64_t)HYPE_DSDT_AML_PCI_WINDOW_MAX) {
        return -1;
    }
    if (total > buf_size) {
        return -1;
    }

    for (i = 0; i < buf_size; i++) {
        buf[i] = 0;
    }

    out->xsdt_offset = 0;
    out->xsdt_length = xsdt_length;
    out->fadt_offset = out->xsdt_offset + xsdt_length;
    out->fadt_length = fadt_length;
    out->madt_offset = out->fadt_offset + fadt_length;
    out->madt_length = madt_length;
    out->mcfg_offset = out->madt_offset + madt_length;
    out->mcfg_length = mcfg_length;
    out->dsdt_offset = out->mcfg_offset + mcfg_length;
    out->dsdt_length = dsdt_length;
    out->total_length = out->dsdt_offset + dsdt_length;

    /* DSDT: SDT header + the compiled AML body from devices/dsdt.asl
     * (devices/dsdt_aml.h). M4-6b2: the body declares the PCI host bridge
     * (_SB.PCI0) with a _PRT so an ACPI-mode kernel can route PCI device
     * interrupts (notably AHCI INTA -> GSI 16) via the I/O APIC -- without it
     * the AHCI driver fails to probe ("PCI INT A: no GSI"). fill_header sets
     * the length (already includes the body) and the fw_cfg table-loader
     * recomputes the checksum over header+body. */
    {
        uint8_t *dsdt = buf + out->dsdt_offset;
        uint32_t j;
        fill_header((hype_acpi_sdt_header_t *)dsdt, "DSDT", out->dsdt_length, 2, "HYPEDSDT");
        for (j = 0; j < HYPE_DSDT_AML_BODY_LEN; j++) {
            dsdt[sizeof(hype_acpi_sdt_header_t) + j] = hype_dsdt_aml_body[j];
        }
        /*
         * #355: point the PCI0 _CRS 32-bit window at THIS VM's RAM top.
         *
         * The blob says 0x80000000 because that is where the default 2048 MiB of guest RAM ends.
         * With mem_mb configurable to 3072 the declared bridge window would otherwise overlap the
         * VM's own RAM. Only the base moves; the top stays one byte below the I/O APIC, so the
         * length follows from the two.
         *
         * Patched in the COPY, never in hype_dsdt_aml_body, which is const and shared by every VM.
         * The offsets come from devices/dsdt_aml.h, derived by tools/gen-dsdt-aml.sh from the
         * compiled AML, so editing devices/dsdt.asl cannot move the field out from under this.
         */
        {
            uint8_t *body = dsdt + sizeof(hype_acpi_sdt_header_t);
            uint32_t base = (uint32_t)cfg->pci_window_base;
            uint32_t len = HYPE_DSDT_AML_PCI_WINDOW_MAX - base + 1u;
            put_le32_at(body + HYPE_DSDT_AML_PCI_WINDOW_MIN_OFF, base);
            put_le32_at(body + HYPE_DSDT_AML_PCI_WINDOW_LEN_OFF, len);
        }
    }

    /* FADT ("FACP") */
    {
        hype_acpi_fadt_t *fadt = (hype_acpi_fadt_t *)(buf + out->fadt_offset);
        fill_header(&fadt->header, "FACP", out->fadt_length, 6, "HYPEFADT");
        fadt->sci_interrupt = (uint16_t)cfg->sci_interrupt;
        fadt->flags = HYPE_ACPI_FADT_WBINVD | HYPE_ACPI_FADT_PWR_BUTTON | HYPE_ACPI_FADT_SLP_BUTTON |
                      HYPE_ACPI_FADT_HW_REDUCED_ACPI;
        /* Dsdt/X_Dsdt: pre-filled with DSDT's offset *within this same
         * blob* (both point at "etc/acpi/tables", the same src_file the
         * loader script's ADD_POINTER command adds its allocated base
         * to) -- not a final address. Legacy 32-bit Dsdt truncation is
         * safe here since this whole blob is a handful of KB. */
        fadt->dsdt = out->dsdt_offset;
        fadt->x_dsdt = out->dsdt_offset;
        /*
         * #318: tell the guest WHERE the RTC century lives. hype's CMOS model writes the
         * century to register 0x32 (devices/cmos.h), but a zero FADT century field means "no
         * century register exists", so a guest reads only the two-digit year and has to guess
         * the century. OpenBSD's inittodr rejects the result -- observed on real hardware as
         * "WARNING: CHECK AND RESET THE DATE!" right after the kernel mounted root, with 245k
         * polls of 0x70/0x71 alongside. Same shape as #303: a correct device model that the
         * guest could not find because a table field was left at zero.
         */
        fadt->century = HYPE_CMOS_REG_CENTURY;
        /* M8-6: SleepControl/SleepStatus left zeroed. This guest is UEFI-booted,
         * so its OS powers off via EFI ResetSystem -> OVMF, which on QEMU uses the
         * classic ACPI PM1a_CNT register (I/O 0x604) with SLP_EN, NOT the
         * reduced-hardware SleepControl. hype detects that PM1a_CNT write directly
         * in the IOIO path (boot/main.c) and posts an S5 lifecycle event; \_S5 is
         * declared in the DSDT so the SLP_TYP value is available. */
    }

    /* MADT ("APIC") */
    {
        uint8_t *p = buf + out->madt_offset;
        hype_acpi_madt_header_t *madt_hdr = (hype_acpi_madt_header_t *)p;
        uint32_t local_offset = (uint32_t)sizeof(hype_acpi_madt_header_t);
        hype_acpi_madt_io_apic_t *ioapic;
        hype_acpi_madt_interrupt_override_t *iso;

        fill_header(&madt_hdr->header, "APIC", out->madt_length, 4, "HYPEMADT");
        madt_hdr->local_apic_address = cfg->local_apic_address;
        madt_hdr->flags = HYPE_ACPI_MADT_PCAT_COMPAT;

        for (i = 0; i < cfg->cpu_count; i++) {
            hype_acpi_madt_local_apic_t *lapic = (hype_acpi_madt_local_apic_t *)(p + local_offset);
            lapic->header.type = HYPE_ACPI_MADT_TYPE_LOCAL_APIC;
            lapic->header.length = (uint8_t)sizeof(*lapic);
            lapic->processor_id = (uint8_t)i;
            lapic->apic_id = cfg->apic_ids[i];
            lapic->flags = HYPE_ACPI_MADT_LOCAL_APIC_ENABLED;
            local_offset += (uint32_t)sizeof(*lapic);
        }

        ioapic = (hype_acpi_madt_io_apic_t *)(p + local_offset);
        ioapic->header.type = HYPE_ACPI_MADT_TYPE_IO_APIC;
        ioapic->header.length = (uint8_t)sizeof(*ioapic);
        ioapic->io_apic_id = cfg->io_apic_id;
        ioapic->reserved = 0;
        ioapic->io_apic_address = cfg->io_apic_address;
        ioapic->global_irq_base = cfg->io_apic_gsi_base;
        local_offset += (uint32_t)sizeof(*ioapic);

        /* Standard PC-compatible ISA IRQ0 (PIT) -> GSI2 override -- the
         * PIT is wired to IOAPIC pin 2, not pin 0, on every real/QEMU
         * PC-compatible chipset; without this override, an OS assuming
         * identity IRQ==GSI mapping would program the wrong pin. */
        iso = (hype_acpi_madt_interrupt_override_t *)(p + local_offset);
        iso->header.type = HYPE_ACPI_MADT_TYPE_INTERRUPT_OVERRIDE;
        iso->header.length = (uint8_t)sizeof(*iso);
        iso->bus = 0;
        iso->source_irq = 0;
        iso->global_irq = 2;
        iso->flags = 0; /* conforms to bus specification: edge-triggered, active-high */
    }

    /* MCFG */
    {
        uint8_t *p = buf + out->mcfg_offset;
        hype_acpi_mcfg_header_t *mcfg_hdr = (hype_acpi_mcfg_header_t *)p;
        hype_acpi_mcfg_allocation_t *alloc =
            (hype_acpi_mcfg_allocation_t *)(p + sizeof(hype_acpi_mcfg_header_t));

        fill_header(&mcfg_hdr->header, "MCFG", out->mcfg_length, 1, "HYPEMCFG");
        alloc->base_address = cfg->mcfg_base_address;
        alloc->pci_segment = cfg->pci_segment;
        alloc->start_bus = cfg->pci_start_bus;
        alloc->end_bus = cfg->pci_end_bus;
        alloc->reserved = 0;
    }

    /* XSDT -- built last so FADT/MADT/MCFG's offsets are already known;
     * each entry pre-filled with that table's offset within this same
     * blob, same not-a-final-address convention as FADT's Dsdt/X_Dsdt
     * above. Written via write_le64() rather than a uint64_t* cast --
     * see that helper's own comment on why. */
    {
        uint8_t *entries = buf + out->xsdt_offset + sizeof(hype_acpi_sdt_header_t);
        hype_acpi_sdt_header_t *xsdt = (hype_acpi_sdt_header_t *)(buf + out->xsdt_offset);

        fill_header(xsdt, "XSDT", out->xsdt_length, 1, "HYPEXSDT");
        write_le64(entries + 0, out->fadt_offset);
        write_le64(entries + 8, out->madt_offset);
        write_le64(entries + 16, out->mcfg_offset);
    }

    return 0;
}
