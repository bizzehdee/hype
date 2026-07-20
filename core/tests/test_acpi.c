#include <stdio.h>
#include <string.h>
#include "../../devices/acpi.h"
#include "../../devices/dsdt_aml.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

#define CHECK_MEM(desc, expected, actual, len) \
    do { \
        if (memcmp((expected), (actual), (len)) != 0) { \
            printf("FAIL: %s: memory mismatch\n", (desc)); \
            failures++; \
        } \
    } while (0)

static void test_checksum_makes_sum_zero(void) {
    uint8_t bytes[] = {0x10, 0x20, 0x30, 0x00}; /* last byte is the checksum slot, currently 0 */
    uint8_t sum;
    uint32_t i;

    bytes[3] = hype_acpi_checksum(bytes, sizeof(bytes));

    sum = 0;
    for (i = 0; i < sizeof(bytes); i++) {
        sum = (uint8_t)(sum + bytes[i]);
    }
    CHECK_HEX("full range now sums to 0 mod 256", 0, sum);
}

static void test_checksum_all_zero_is_zero(void) {
    uint8_t bytes[4] = {0, 0, 0, 0};
    CHECK_HEX("checksum of all-zero bytes is 0", 0, hype_acpi_checksum(bytes, sizeof(bytes)));
}

static void test_build_rsdp(void) {
    hype_acpi_rsdp_t rsdp;
    static const char expected_sig[8] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};

    hype_acpi_build_rsdp(&rsdp, 0x1234);

    CHECK_MEM("signature", expected_sig, rsdp.signature, 8);
    CHECK_HEX("checksum left 0 for guest ADD_CHECKSUM patch", 0, rsdp.checksum);
    CHECK_HEX("revision is 2 (ACPI 2.0+)", 2, rsdp.revision);
    CHECK_HEX("rsdt_address unused/0", 0, rsdp.rsdt_address);
    CHECK_HEX("length is 36", 36, rsdp.length);
    CHECK_HEX("xsdt_address holds the blob-relative offset, not a real address", 0x1234, rsdp.xsdt_address);
    CHECK_HEX("extended_checksum left 0 for guest ADD_CHECKSUM patch", 0, rsdp.extended_checksum);
}

static hype_acpi_config_t make_config(uint8_t cpu_count) {
    hype_acpi_config_t cfg;
    uint32_t i;

    for (i = 0; i < HYPE_ACPI_MAX_CPUS; i++) {
        cfg.apic_ids[i] = (uint8_t)i;
    }
    cfg.cpu_count = cpu_count;
    cfg.local_apic_address = 0xFEE00000u;
    cfg.io_apic_id = 1;
    cfg.io_apic_address = 0xFEC00000u;
    cfg.io_apic_gsi_base = 0;
    cfg.mcfg_base_address = 0xE0000000ULL;
    cfg.pci_segment = 0;
    cfg.pci_start_bus = 0;
    cfg.pci_end_bus = 255;
    cfg.sci_interrupt = 9;
    return cfg;
}

static void test_rejects_zero_cpus(void) {
    static uint8_t buf[4096];
    hype_acpi_layout_t layout;
    hype_acpi_config_t cfg = make_config(0);
    int rc = hype_acpi_build_tables_blob(buf, sizeof(buf), &cfg, &layout);
    if (rc == 0) {
        printf("FAIL: zero cpu_count should be rejected\n");
        failures++;
    }
}

static void test_rejects_too_many_cpus(void) {
    static uint8_t buf[4096];
    hype_acpi_layout_t layout;
    hype_acpi_config_t cfg = make_config((uint8_t)(HYPE_ACPI_MAX_CPUS + 1));
    int rc = hype_acpi_build_tables_blob(buf, sizeof(buf), &cfg, &layout);
    if (rc == 0) {
        printf("FAIL: cpu_count beyond HYPE_ACPI_MAX_CPUS should be rejected\n");
        failures++;
    }
}

static void test_rejects_buffer_too_small(void) {
    static uint8_t buf[8]; /* nowhere near enough for even the XSDT header alone */
    hype_acpi_layout_t layout;
    hype_acpi_config_t cfg = make_config(2);
    int rc = hype_acpi_build_tables_blob(buf, sizeof(buf), &cfg, &layout);
    if (rc == 0) {
        printf("FAIL: undersized buffer should be rejected\n");
        failures++;
    }
}

static void test_build_tables_blob_layout(void) {
    static uint8_t buf[4096];
    hype_acpi_layout_t layout;
    hype_acpi_config_t cfg = make_config(2);
    int rc = hype_acpi_build_tables_blob(buf, sizeof(buf), &cfg, &layout);
    hype_acpi_sdt_header_t *xsdt_hdr;
    hype_acpi_fadt_t *fadt;
    hype_acpi_madt_header_t *madt_hdr;
    hype_acpi_madt_local_apic_t *lapic0, *lapic1;
    hype_acpi_madt_io_apic_t *ioapic;
    hype_acpi_madt_interrupt_override_t *iso;
    hype_acpi_mcfg_header_t *mcfg_hdr;
    hype_acpi_mcfg_allocation_t *alloc;
    uint8_t *entries;

    if (rc != 0) {
        printf("FAIL: expected success, got failure\n");
        failures++;
        return;
    }

    CHECK_HEX("xsdt at offset 0", 0, layout.xsdt_offset);
    CHECK_HEX("fadt follows xsdt", layout.xsdt_offset + layout.xsdt_length, layout.fadt_offset);
    CHECK_HEX("madt follows fadt", layout.fadt_offset + layout.fadt_length, layout.madt_offset);
    CHECK_HEX("mcfg follows madt", layout.madt_offset + layout.madt_length, layout.mcfg_offset);
    CHECK_HEX("dsdt follows mcfg", layout.mcfg_offset + layout.mcfg_length, layout.dsdt_offset);
    CHECK_HEX("total_length covers everything", layout.dsdt_offset + layout.dsdt_length,
              layout.total_length);

    /* XSDT */
    xsdt_hdr = (hype_acpi_sdt_header_t *)(buf + layout.xsdt_offset);
    CHECK_MEM("xsdt signature", "XSDT", xsdt_hdr->signature, 4);
    CHECK_HEX("xsdt length field matches layout", layout.xsdt_length, xsdt_hdr->length);
    CHECK_HEX("xsdt checksum left 0", 0, xsdt_hdr->checksum);
    entries = buf + layout.xsdt_offset + sizeof(hype_acpi_sdt_header_t);
    {
        uint64_t fadt_entry = 0, madt_entry = 0, mcfg_entry = 0;
        int i;
        for (i = 7; i >= 0; i--) {
            fadt_entry = (fadt_entry << 8) | entries[0 + i];
            madt_entry = (madt_entry << 8) | entries[8 + i];
            mcfg_entry = (mcfg_entry << 8) | entries[16 + i];
        }
        CHECK_HEX("xsdt entry 0 -> fadt offset", layout.fadt_offset, fadt_entry);
        CHECK_HEX("xsdt entry 1 -> madt offset", layout.madt_offset, madt_entry);
        CHECK_HEX("xsdt entry 2 -> mcfg offset", layout.mcfg_offset, mcfg_entry);
    }

    /* FADT */
    fadt = (hype_acpi_fadt_t *)(buf + layout.fadt_offset);
    CHECK_MEM("fadt signature", "FACP", fadt->header.signature, 4);
    CHECK_HEX("fadt checksum left 0", 0, fadt->header.checksum);
    CHECK_HEX("fadt sci_interrupt", 9, fadt->sci_interrupt);
    CHECK_HEX("fadt has HW_REDUCED_ACPI set", 1,
              (fadt->flags & HYPE_ACPI_FADT_HW_REDUCED_ACPI) != 0);
    CHECK_HEX("fadt.dsdt holds dsdt's blob-relative offset", layout.dsdt_offset, fadt->dsdt);
    CHECK_HEX("fadt.x_dsdt holds dsdt's blob-relative offset", layout.dsdt_offset, fadt->x_dsdt);

    /* MADT */
    madt_hdr = (hype_acpi_madt_header_t *)(buf + layout.madt_offset);
    CHECK_MEM("madt signature", "APIC", madt_hdr->header.signature, 4);
    CHECK_HEX("madt local_apic_address", 0xFEE00000u, madt_hdr->local_apic_address);
    lapic0 = (hype_acpi_madt_local_apic_t *)(buf + layout.madt_offset + sizeof(hype_acpi_madt_header_t));
    lapic1 = (hype_acpi_madt_local_apic_t *)((uint8_t *)lapic0 + sizeof(*lapic0));
    CHECK_HEX("lapic0 type", HYPE_ACPI_MADT_TYPE_LOCAL_APIC, lapic0->header.type);
    CHECK_HEX("lapic0 processor_id", 0, lapic0->processor_id);
    CHECK_HEX("lapic0 apic_id", 0, lapic0->apic_id);
    CHECK_HEX("lapic0 enabled", 1, (lapic0->flags & HYPE_ACPI_MADT_LOCAL_APIC_ENABLED) != 0);
    CHECK_HEX("lapic1 processor_id", 1, lapic1->processor_id);
    CHECK_HEX("lapic1 apic_id", 1, lapic1->apic_id);
    ioapic = (hype_acpi_madt_io_apic_t *)((uint8_t *)lapic1 + sizeof(*lapic1));
    CHECK_HEX("ioapic type", HYPE_ACPI_MADT_TYPE_IO_APIC, ioapic->header.type);
    CHECK_HEX("ioapic id", 1, ioapic->io_apic_id);
    CHECK_HEX("ioapic address", 0xFEC00000u, ioapic->io_apic_address);
    iso = (hype_acpi_madt_interrupt_override_t *)((uint8_t *)ioapic + sizeof(*ioapic));
    CHECK_HEX("iso type", HYPE_ACPI_MADT_TYPE_INTERRUPT_OVERRIDE, iso->header.type);
    CHECK_HEX("iso source_irq", 0, iso->source_irq);
    CHECK_HEX("iso global_irq is GSI2", 2, iso->global_irq);

    /* MCFG */
    mcfg_hdr = (hype_acpi_mcfg_header_t *)(buf + layout.mcfg_offset);
    CHECK_MEM("mcfg signature", "MCFG", mcfg_hdr->header.signature, 4);
    alloc = (hype_acpi_mcfg_allocation_t *)(buf + layout.mcfg_offset + sizeof(hype_acpi_mcfg_header_t));
    CHECK_HEX("mcfg allocation base_address", 0xE0000000ULL, alloc->base_address);
    CHECK_HEX("mcfg allocation end_bus", 255, alloc->end_bus);

    /* DSDT: SDT header followed by the compiled AML body (M4-6b2 _PRT). */
    {
        hype_acpi_sdt_header_t *dsdt = (hype_acpi_sdt_header_t *)(buf + layout.dsdt_offset);
        CHECK_MEM("dsdt signature", "DSDT", dsdt->signature, 4);
        CHECK_HEX("dsdt length = header + AML body", sizeof(hype_acpi_sdt_header_t) + HYPE_DSDT_AML_BODY_LEN,
                  layout.dsdt_length);
        CHECK_HEX("dsdt SDT-header length field matches", layout.dsdt_length, dsdt->length);
        /* the AML body is copied verbatim after the header */
        CHECK_MEM("dsdt AML body copied after header", hype_dsdt_aml_body,
                  (uint8_t *)dsdt + sizeof(hype_acpi_sdt_header_t), HYPE_DSDT_AML_BODY_LEN);
    }
}

int main(void) {
    test_checksum_makes_sum_zero();
    test_checksum_all_zero_is_zero();
    test_build_rsdp();
    test_rejects_zero_cpus();
    test_rejects_too_many_cpus();
    test_rejects_buffer_too_small();
    test_build_tables_blob_layout();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
