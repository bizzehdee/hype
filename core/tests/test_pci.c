#include <stdio.h>
#include "../../devices/pci.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_decode_ecam_offset(void) {
    /* bus=1, device=17 (0x11), function=3, register=0x0C ->
     * offset = (1<<20) | (17<<15) | (3<<12) | 0x0C
     *        = 0x100000 | 0x88000 | 0x3000 | 0x0C = 0x18B00C */
    hype_pci_ecam_addr_t addr;

    hype_pci_decode_ecam_offset(0x18B00Cu, &addr);

    CHECK_HEX("bus", 1, addr.bus);
    CHECK_HEX("device", 17, addr.device);
    CHECK_HEX("function", 3, addr.function);
    CHECK_HEX("register", 0x0C, addr.register_offset);
}

static void test_absent_device_reads_all_ones(void) {
    hype_pci_t pci;
    hype_pci_ecam_addr_t addr = {0, 5, 0, 0};
    uint32_t value;

    hype_pci_reset(&pci);

    hype_pci_config_read(&pci, &addr, 4, &value);
    CHECK_HEX("4-byte read", 0xFFFFFFFFu, value);

    hype_pci_config_read(&pci, &addr, 2, &value);
    CHECK_HEX("2-byte read", 0xFFFFu, value);

    hype_pci_config_read(&pci, &addr, 1, &value);
    CHECK_HEX("1-byte read", 0xFFu, value);
}

static void test_non_bus_zero_or_non_function_zero_reads_absent(void) {
    hype_pci_t pci;
    hype_pci_ecam_addr_t addr_bus1 = {1, 0, 0, 0};
    hype_pci_ecam_addr_t addr_func1 = {0, 0, 1, 0};
    uint32_t value;

    hype_pci_reset(&pci);
    hype_pci_add_device(&pci, 0, 0x1234, 0x5678, 0x06, 0x00, 0x00);

    hype_pci_config_read(&pci, &addr_bus1, 4, &value);
    CHECK_HEX("bus 1 reads absent", 0xFFFFFFFFu, value);

    hype_pci_config_read(&pci, &addr_func1, 4, &value);
    CHECK_HEX("function 1 reads absent", 0xFFFFFFFFu, value);
}

static void test_multifunction_device(void) {
    hype_pci_t pci;
    hype_pci_ecam_addr_t function0_header = {0, 31, 0, 0x0E};
    hype_pci_ecam_addr_t function1 = {0, 31, 1, 0x00};
    hype_pci_ecam_addr_t function2_id = {0, 31, 2, 0x00};
    hype_pci_ecam_addr_t function2_class = {0, 31, 2, 0x08};
    hype_pci_ecam_addr_t function2_bar5 = {0, 31, 2, 0x24};
    hype_pci_ecam_addr_t function2_command = {0, 31, 2, 0x04};
    uint32_t value;

    hype_pci_reset(&pci);
    CHECK_HEX("function requires function 0", -1,
              hype_pci_add_function(&pci, 31, 2, 0x8086u, 0x2922u, 0x01, 0x06, 0x01));
    hype_pci_add_device(&pci, 31, 0x8086u, 0x2918u, 0x06, 0x01, 0x00);
    CHECK_HEX("register ICH9 AHCI function", 0,
              hype_pci_add_function(&pci, 31, 2, 0x8086u, 0x2922u, 0x01, 0x06, 0x01));

    hype_pci_config_read(&pci, &function0_header, 1, &value);
    CHECK_HEX("function 0 advertises multifunction", 0x80u, value & 0x80u);
    hype_pci_config_read(&pci, &function1, 4, &value);
    CHECK_HEX("unregistered function remains absent", 0xFFFFFFFFu, value);
    hype_pci_config_read(&pci, &function2_id, 4, &value);
    CHECK_HEX("function 2 vendor/device", 0x29228086u, value);
    hype_pci_config_read(&pci, &function2_class, 4, &value);
    CHECK_HEX("function 2 AHCI class", 0x01060100u, value);
    CHECK_HEX("function 2 MSI starts absent", 0, hype_pci_function_msi_enabled(&pci, 31, 2));

    hype_pci_set_function_bar_size(&pci, 31, 2, 5, 0x1000u);
    hype_pci_config_write(&pci, &function2_bar5, 4, 0xE0100123u);
    CHECK_HEX("function 2 BAR5 programmed", 0xE0100000u,
              hype_pci_get_function_bar_value(&pci, 31, 2, 5));
    hype_pci_config_write(&pci, &function2_command, 2, 0x0006u);
    CHECK_HEX("function 2 memory enabled", 1,
              hype_pci_function_memory_space_enabled(&pci, 31, 2));
    CHECK_HEX("function 2 bus master enabled", 1,
              hype_pci_function_bus_master_enabled(&pci, 31, 2));
    hype_pci_set_function_interrupt(&pci, 31, 2, 1, 16);
    CHECK_HEX("function 2 interrupt line", 16,
              hype_pci_get_function_interrupt_line(&pci, 31, 2));

    hype_pci_set_function_msi_capability(&pci, 31, 2);
    CHECK_HEX("function 2 MSI starts disabled", 0, hype_pci_function_msi_enabled(&pci, 31, 2));
    hype_pci_config_write(&pci, &(hype_pci_ecam_addr_t){0, 31, 2, HYPE_PCI_MSI_CAP_OFFSET}, 2, 0u);
    hype_pci_config_read(&pci, &(hype_pci_ecam_addr_t){0, 31, 2, HYPE_PCI_MSI_CAP_OFFSET}, 1, &value);
    CHECK_HEX("function 2 MSI capability ID is hardware-owned", HYPE_PCI_CAP_ID_MSI, value);
    hype_pci_config_write(&pci, &(hype_pci_ecam_addr_t){0, 31, 2, HYPE_PCI_MSI_DATA_OFFSET}, 2, 0x0062u);
    hype_pci_config_write(&pci, &(hype_pci_ecam_addr_t){0, 31, 2, HYPE_PCI_MSI_CONTROL_OFFSET}, 2,
                          HYPE_PCI_MSI_CONTROL_ENABLE);
    CHECK_HEX("function 2 MSI enabled", 1, hype_pci_function_msi_enabled(&pci, 31, 2));
    CHECK_HEX("function 2 MSI vector", 0x62u, hype_pci_function_msi_vector(&pci, 31, 2));
    {   /* #512: destination from the guest-programmed MSI address. Physical fixed first
         * (RH=0, DM=0), then logical lowest-priority (RH=1, DM=1). */
        int logical = 1;
        hype_pci_config_write(&pci, &(hype_pci_ecam_addr_t){0, 31, 2, HYPE_PCI_MSI_ADDR_OFFSET},
                              4, 0xFEE01000u);
        CHECK_HEX("function 2 MSI dest physical", 0x01u,
                  hype_pci_function_msi_dest(&pci, 31, 2, &logical));
        CHECK_HEX("function 2 MSI dest mode physical", 0, logical);
        hype_pci_config_write(&pci, &(hype_pci_ecam_addr_t){0, 31, 2, HYPE_PCI_MSI_ADDR_OFFSET},
                              4, 0xFEE0300Cu);
        CHECK_HEX("function 2 MSI dest logical", 0x03u,
                  hype_pci_function_msi_dest(&pci, 31, 2, &logical));
        CHECK_HEX("function 2 MSI dest mode logical", 1, logical);
        /* RH alone is not logical mode: DM clear means physical, whatever RH says. */
        hype_pci_config_write(&pci, &(hype_pci_ecam_addr_t){0, 31, 2, HYPE_PCI_MSI_ADDR_OFFSET},
                              4, 0xFEE02008u);
        CHECK_HEX("function 2 MSI dest RH-only", 0x02u,
                  hype_pci_function_msi_dest(&pci, 31, 2, &logical));
        CHECK_HEX("function 2 MSI dest mode RH-only", 0, logical);
        CHECK_HEX("function 2 MSI dest null-logical tolerated", 0x02u,
                  hype_pci_function_msi_dest(&pci, 31, 2, (int *)0));
    }
    CHECK_HEX("function 2 raw config is exposed", 0x22u,
              hype_pci_function_config(&pci, 31, 2)[0x02]);

    /* Both a nonexistent but in-range function and an invalid function must
     * be inert. This covers the exact no-device cases PCI enumeration hits. */
    hype_pci_set_function_bar_size(&pci, 31, 3, 5, 0x1000u);
    hype_pci_set_function_bar_size(&pci, 31, HYPE_PCI_MAX_FUNCTIONS, 5, 0x1000u);
    hype_pci_set_function_interrupt(&pci, 31, 3, 1, 16);
    hype_pci_set_function_interrupt(&pci, 31, HYPE_PCI_MAX_FUNCTIONS, 1, 16);
    hype_pci_set_function_msi_capability(&pci, 31, 3);
    hype_pci_set_function_msi_capability(&pci, 31, HYPE_PCI_MAX_FUNCTIONS);
    CHECK_HEX("absent function MSI disabled", 0, hype_pci_function_msi_enabled(&pci, 31, 3));
    CHECK_HEX("invalid function MSI disabled", 0,
              hype_pci_function_msi_enabled(&pci, 31, HYPE_PCI_MAX_FUNCTIONS));
    CHECK_HEX("absent function MSI vector", 0, hype_pci_function_msi_vector(&pci, 31, 3));
    CHECK_HEX("invalid function MSI vector", 0,
              hype_pci_function_msi_vector(&pci, 31, HYPE_PCI_MAX_FUNCTIONS));
    CHECK_HEX("absent function MSI dest", 0, hype_pci_function_msi_dest(&pci, 31, 3, (int *)0));
    CHECK_HEX("invalid function MSI dest", 0,
              hype_pci_function_msi_dest(&pci, 31, HYPE_PCI_MAX_FUNCTIONS, (int *)0));
    CHECK_HEX("absent function interrupt line", 0, hype_pci_get_function_interrupt_line(&pci, 31, 3));
    CHECK_HEX("invalid function interrupt line", 0,
              hype_pci_get_function_interrupt_line(&pci, 31, HYPE_PCI_MAX_FUNCTIONS));

    CHECK_HEX("function zero cannot be re-added", -1,
              hype_pci_add_function(&pci, 31, 0, 0, 0, 0, 0, 0));
    CHECK_HEX("function eight is out of range", -1,
              hype_pci_add_function(&pci, 31, HYPE_PCI_MAX_FUNCTIONS, 0, 0, 0, 0, 0));
    CHECK_HEX("out-of-range device rejects function", -1,
              hype_pci_add_function(&pci, HYPE_PCI_MAX_DEVICES, 2, 0, 0, 0, 0, 0));
    CHECK_HEX("invalid function has no config", 0,
              hype_pci_function_config(&pci, 31, HYPE_PCI_MAX_FUNCTIONS) != 0);
    CHECK_HEX("absent function has no config", 0, hype_pci_function_config(&pci, 31, 3) != 0);
    CHECK_HEX("absent function memory is disabled", 0,
              hype_pci_function_memory_space_enabled(&pci, 31, 3));
    CHECK_HEX("absent function bus master is disabled", 0,
              hype_pci_function_bus_master_enabled(&pci, 31, 3));
}

static void test_add_device_populates_header(void) {
    hype_pci_t pci;
    hype_pci_ecam_addr_t addr;
    uint32_t value;

    hype_pci_reset(&pci);
    hype_pci_add_device(&pci, 0, HYPE_PCI_VENDOR_ID_HYPE, 0x0000u, 0x06, 0x00, 0x00);

    addr.bus = 0;
    addr.device = 0;
    addr.function = 0;

    addr.register_offset = 0x00;
    hype_pci_config_read(&pci, &addr, 4, &value);
    CHECK_HEX("vendor/device ID", ((uint32_t)0x0000u << 16) | HYPE_PCI_VENDOR_ID_HYPE, value);

    addr.register_offset = 0x08;
    hype_pci_config_read(&pci, &addr, 4, &value);
    /* revision=0x00, interface=0x00, sub=0x00, base=0x06 -> LE dword 0x06000000 */
    CHECK_HEX("revision/class code", 0x06000000u, value);

    addr.register_offset = 0x0E;
    hype_pci_config_read(&pci, &addr, 1, &value);
    CHECK_HEX("header type (single-function)", 0x00, value);
}

static void test_ahci_class_code(void) {
    hype_pci_t pci;
    hype_pci_ecam_addr_t addr = {0, 1, 0, 0x08};
    uint32_t value;

    hype_pci_reset(&pci);
    /* Mass storage(0x01) / SATA(0x06) / AHCI 1.0(0x01) -- the real,
     * well-known class code every AHCI-compliant OS driver binds
     * against, independent of vendor/device ID. */
    hype_pci_add_device(&pci, 1, HYPE_PCI_VENDOR_ID_HYPE, 0x0001u, 0x01, 0x06, 0x01);

    hype_pci_config_read(&pci, &addr, 4, &value);
    CHECK_HEX("AHCI class code (0x010601xx)", 0x01060100u, value);
}

static void test_set_interrupt_pin_and_line(void) {
    hype_pci_t pci;
    hype_pci_ecam_addr_t line = {0, 1, 0, 0x3C};
    hype_pci_ecam_addr_t both = {0, 1, 0, 0x3C};
    uint32_t value;

    hype_pci_reset(&pci);
    hype_pci_add_device(&pci, 1, HYPE_PCI_VENDOR_ID_HYPE, 0x0005u, 0x01, 0x06, 0x01);

    /* Before set: Interrupt Pin/Line both read 0 (function uses no
     * interrupt) -- exactly the state that left a no-ACPI guest with no
     * IRQ to request (M4-6d2). */
    hype_pci_config_read(&pci, &line, 1, &value);
    CHECK_HEX("Interrupt Line defaults to 0", 0x00u, value);

    hype_pci_set_interrupt(&pci, 1, 1u, 5u);

    hype_pci_config_read(&pci, &line, 1, &value);
    CHECK_HEX("Interrupt Line = 5 after set", 0x05u, value);
    /* 0x3C..0x3D as a 16-bit read: line in the low byte, pin (INTA=1) in
     * the high byte -- the layout a guest reads to route the device. */
    hype_pci_config_read(&pci, &both, 2, &value);
    CHECK_HEX("Interrupt Line|Pin<<8 = 0x0105", 0x0105u, value);
}

static void test_set_interrupt_ignores_out_of_range_device(void) {
    hype_pci_t pci;
    hype_pci_reset(&pci);
    /* Must not write out of bounds for an invalid device number. */
    hype_pci_set_interrupt(&pci, (uint8_t)HYPE_PCI_MAX_DEVICES, 1u, 5u);
    hype_pci_set_interrupt(&pci, 0xFFu, 1u, 5u);
}

static void test_msi_capability_and_guest_programming(void) {
    hype_pci_t pci;
    hype_pci_ecam_addr_t status = {0, 1, 0, 0x06};
    hype_pci_ecam_addr_t cap_ptr = {0, 1, 0, HYPE_PCI_CAP_PTR_OFFSET};
    hype_pci_ecam_addr_t cap_header = {0, 1, 0, HYPE_PCI_MSI_CAP_OFFSET};
    hype_pci_ecam_addr_t control = {0, 1, 0, HYPE_PCI_MSI_CONTROL_OFFSET};
    hype_pci_ecam_addr_t data = {0, 1, 0, HYPE_PCI_MSI_DATA_OFFSET};
    uint32_t value;

    hype_pci_reset(&pci);
    hype_pci_add_device(&pci, 1, HYPE_PCI_VENDOR_ID_HYPE, 0x0005u, 0x01, 0x06, 0x01);
    hype_pci_set_msi_capability(&pci, 1);

    hype_pci_config_read(&pci, &status, 2, &value);
    CHECK_HEX("Status advertises capability list", HYPE_PCI_STATUS_CAPABILITIES_LIST,
              value & HYPE_PCI_STATUS_CAPABILITIES_LIST);
    hype_pci_config_read(&pci, &cap_ptr, 1, &value);
    CHECK_HEX("Capabilities Pointer names MSI", HYPE_PCI_MSI_CAP_OFFSET, value);
    hype_pci_config_read(&pci, &cap_header, 2, &value);
    CHECK_HEX("MSI capability ID and end-of-list", HYPE_PCI_CAP_ID_MSI, value);
    CHECK_HEX("MSI is disabled until guest enables it", 0, hype_pci_msi_enabled(&pci, 1));

    hype_pci_config_write(&pci, &data, 2, 0x0062u);
    hype_pci_config_write(&pci, &control, 2, HYPE_PCI_MSI_CONTROL_ENABLE);
    CHECK_HEX("guest can enable MSI", 1, hype_pci_msi_enabled(&pci, 1));
    CHECK_HEX("MSI vector comes from message data", 0x62u, hype_pci_msi_vector(&pci, 1));

    /* Device-owned discovery fields survive a broad guest config write. */
    hype_pci_config_write(&pci, &status, 2, 0u);
    hype_pci_config_write(&pci, &cap_ptr, 1, 0u);
    hype_pci_config_read(&pci, &status, 2, &value);
    CHECK_HEX("Status capability bit stays hardware-owned", HYPE_PCI_STATUS_CAPABILITIES_LIST,
              value & HYPE_PCI_STATUS_CAPABILITIES_LIST);
    hype_pci_config_read(&pci, &cap_ptr, 1, &value);
    CHECK_HEX("Capabilities Pointer stays hardware-owned", HYPE_PCI_MSI_CAP_OFFSET, value);
}

static void test_bar_sizing_protocol(void) {
    hype_pci_t pci;
    hype_pci_ecam_addr_t bar0 = {0, 1, 0, 0x10};
    uint32_t value;

    hype_pci_reset(&pci);
    hype_pci_add_device(&pci, 1, HYPE_PCI_VENDOR_ID_HYPE, 0x0001u, 0x01, 0x06, 0x01);
    hype_pci_set_bar_size(&pci, 1, 0, 0x1000u); /* 4KB */

    /* Sizing probe: write all-1s, read back the size mask. */
    hype_pci_config_write(&pci, &bar0, 4, 0xFFFFFFFFu);
    hype_pci_config_read(&pci, &bar0, 4, &value);
    CHECK_HEX("BAR size mask (4KB)", 0xFFFFF000u, value);
    CHECK_HEX("get_bar_value matches", 0xFFFFF000u, hype_pci_get_bar_value(&pci, 1, 0));

    /* Real address programming: guest picks 0xE0100000, low bits below
     * the BAR's own alignment are masked off by the device itself. */
    hype_pci_config_write(&pci, &bar0, 4, 0xE0100123u);
    hype_pci_config_read(&pci, &bar0, 4, &value);
    CHECK_HEX("BAR programmed address (aligned)", 0xE0100000u, value);
}

static void test_io_bar_sizing_protocol(void) {
    hype_pci_t pci;
    hype_pci_ecam_addr_t bar0 = {0, 31, 2, 0x10};
    uint32_t value;

    hype_pci_reset(&pci);
    hype_pci_add_device(&pci, 31, 0x8086u, 0x2918u, 0x06, 0x01, 0x00);
    hype_pci_add_function(&pci, 31, 2, 0x8086u, 0x2922u, 0x01, 0x06, 0x01);
    hype_pci_set_function_io_bar_size(&pci, 31, 2, 0, 8u);

    hype_pci_config_write(&pci, &bar0, 4, 0xFFFFFFFFu);
    hype_pci_config_read(&pci, &bar0, 4, &value);
    CHECK_HEX("I/O BAR size mask keeps type bit", 0xFFFFFFF9u, value);

    hype_pci_config_write(&pci, &bar0, 4, 0x0000D007u);
    hype_pci_config_read(&pci, &bar0, 4, &value);
    CHECK_HEX("I/O BAR aligns and retains type bit", 0x0000D001u, value);
    CHECK_HEX("I/O function BAR value", 0x0000D001u,
              hype_pci_get_function_bar_value(&pci, 31, 2, 0));
}

static void test_ich9_ahci_pci_profile(void) {
    hype_pci_t pci;
    hype_pci_ecam_addr_t cfg = {0, 31, 2, 0};
    uint32_t value;

    hype_pci_reset(&pci);
    hype_pci_add_device(&pci, 31, 0x8086u, 0x2918u, 0x06, 0x01, 0x00);
    CHECK_HEX("ICH9 AHCI requires LPC function zero", 0,
              hype_pci_add_ich9_ahci_function(&pci, 31));
    hype_pci_config_read(&pci, &cfg, 4, &value);
    CHECK_HEX("ICH9 AHCI vendor/device", 0x29228086u, value);
    cfg.register_offset = 0x08;
    hype_pci_config_read(&pci, &cfg, 4, &value);
    CHECK_HEX("ICH9 AHCI revision/class", 0x01060102u, value);
    cfg.register_offset = 0x0C;
    hype_pci_config_read(&pci, &cfg, 2, &value);
    CHECK_HEX("ICH9 cache line and latency", 0x0008u, value);
    cfg.register_offset = 0x34;
    hype_pci_config_read(&pci, &cfg, 1, &value);
    CHECK_HEX("ICH9 capability pointer", 0x80u, value);
    cfg.register_offset = 0x80;
    hype_pci_config_read(&pci, &cfg, 4, &value);
    CHECK_HEX("ICH9 MSI header", 0x0080A805u, value);
    cfg.register_offset = 0xA8;
    hype_pci_config_read(&pci, &cfg, 4, &value);
    CHECK_HEX("ICH9 SATA header", 0x00100012u, value);
    cfg.register_offset = 0xAC;
    hype_pci_config_read(&pci, &cfg, 4, &value);
    CHECK_HEX("ICH9 SATA index-data descriptor", 0x00000048u, value);
    cfg.register_offset = 0x90;
    hype_pci_config_read(&pci, &cfg, 1, &value);
    CHECK_HEX("ICH9 begins in AHCI mode", 0x40u, value);
    cfg.register_offset = 0x24;
    hype_pci_config_write(&pci, &cfg, 4, 0xFFFFFFFFu);
    hype_pci_config_read(&pci, &cfg, 4, &value);
    CHECK_HEX("ICH9 BAR5 is 2KiB memory", 0xFFFFF800u, value);
    cfg.register_offset = 0x20;
    hype_pci_config_write(&pci, &cfg, 4, 0xFFFFFFFFu);
    hype_pci_config_read(&pci, &cfg, 4, &value);
    CHECK_HEX("ICH9 BAR4 is 32-byte I/O", 0xFFFFFFE1u, value);
    hype_pci_config_write(&pci, &(hype_pci_ecam_addr_t){0, 31, 2, 0x8Cu}, 2, 0x0063u);
    hype_pci_config_write(&pci, &(hype_pci_ecam_addr_t){0, 31, 2, 0x82u}, 2, 0x0081u);
    CHECK_HEX("ICH9 64-bit MSI enables", 1, hype_pci_function_msi_enabled(&pci, 31, 2));
    CHECK_HEX("ICH9 64-bit MSI vector", 0x63u, hype_pci_function_msi_vector(&pci, 31, 2));
}

static void test_unimplemented_bar_always_reads_zero(void) {
    hype_pci_t pci;
    hype_pci_ecam_addr_t bar1 = {0, 0, 0, 0x14};
    uint32_t value;

    hype_pci_reset(&pci);
    hype_pci_add_device(&pci, 0, HYPE_PCI_VENDOR_ID_HYPE, 0x0000u, 0x06, 0x00, 0x00);
    /* BAR1 never sized -- hype_pci_set_bar_size() not called for it. */

    hype_pci_config_write(&pci, &bar1, 4, 0xFFFFFFFFu);
    hype_pci_config_read(&pci, &bar1, 4, &value);
    CHECK_HEX("unimplemented BAR reads 0 even after a sizing probe", 0, value);
}

static void test_write_to_absent_device_is_dropped(void) {
    hype_pci_t pci;
    hype_pci_ecam_addr_t addr = {0, 9, 0, 0x00};
    uint32_t value;

    hype_pci_reset(&pci);

    hype_pci_config_write(&pci, &addr, 4, 0xDEADBEEFu);
    hype_pci_config_read(&pci, &addr, 4, &value);
    CHECK_HEX("still reads absent after a write attempt", 0xFFFFFFFFu, value);
}

static void test_memory_space_enable_bit(void) {
    hype_pci_t pci;
    hype_pci_ecam_addr_t command_reg = {0, 0, 0, 0x04};

    hype_pci_reset(&pci);
    hype_pci_add_device(&pci, 0, HYPE_PCI_VENDOR_ID_HYPE, 0x0000u, 0x06, 0x00, 0x00);

    CHECK_HEX("memory space disabled by default", 0, hype_pci_memory_space_enabled(&pci, 0));

    hype_pci_config_write(&pci, &command_reg, 2, 0x0002u);
    CHECK_HEX("memory space enabled after setting bit 1", 1, hype_pci_memory_space_enabled(&pci, 0));
}

/*
 * #372: Bus Master Enable, the bit that decides whether a controller may reach guest memory at all.
 * Emulated devices used to transfer regardless, so a guest driver that forgot it worked here and
 * hung on real hardware -- hype gave a passing result for code that is wrong.
 */
static void test_bus_master_enabled(void) {
    hype_pci_t pci;
    hype_pci_ecam_addr_t command_reg = {0, 0, 0, 0x04};

    hype_pci_reset(&pci);
    hype_pci_add_device(&pci, 0, HYPE_PCI_VENDOR_ID_HYPE, 0x0000u, 0x06, 0x00, 0x00);

    CHECK_HEX("bus master disabled by default", 0, hype_pci_bus_master_enabled(&pci, 0));

    /* Memory-space enable must NOT imply bus master: they are separate bits and a driver commonly
     * sets the first and forgets the second, which is the exact bug this models. */
    hype_pci_config_write(&pci, &command_reg, 2, 0x0002u);
    CHECK_HEX("memory space alone does not enable bus master", 0,
              hype_pci_bus_master_enabled(&pci, 0));
    CHECK_HEX("...and memory space itself is on", 1, hype_pci_memory_space_enabled(&pci, 0));

    hype_pci_config_write(&pci, &command_reg, 2, 0x0006u);
    CHECK_HEX("bus master enabled after setting bit 2", 1, hype_pci_bus_master_enabled(&pci, 0));

    /* A driver unbinding or resetting the device clears it again; hype must follow, which is why
     * the live path re-reads this rather than latching it once. */
    hype_pci_config_write(&pci, &command_reg, 2, 0x0002u);
    CHECK_HEX("bus master clears again when the guest clears the bit", 0,
              hype_pci_bus_master_enabled(&pci, 0));
}

static void test_bus_master_out_of_range_and_absent(void) {
    hype_pci_t pci;

    hype_pci_reset(&pci);
    /* Refusing is the safe answer for a device that is not there: the alternative reads a
     * neighbouring slot's config and authorises a transfer on its behalf. */
    CHECK_HEX("out-of-range device", 0, hype_pci_bus_master_enabled(&pci, HYPE_PCI_MAX_DEVICES));
    CHECK_HEX("absent device", 0, hype_pci_bus_master_enabled(&pci, 3));
}

static void test_add_device_out_of_range_rejected(void) {
    hype_pci_t pci;
    int rc;

    hype_pci_reset(&pci);
    rc = hype_pci_add_device(&pci, HYPE_PCI_MAX_DEVICES, 1, 2, 3, 4, 5);
    CHECK_HEX("out-of-range device number rejected", 1, rc < 0);
}

static void test_set_bar_size_out_of_range_is_a_no_op(void) {
    hype_pci_t pci;

    hype_pci_reset(&pci);
    hype_pci_add_device(&pci, 0, 1, 2, 3, 4, 5);

    /* Neither should crash or affect anything -- just silently ignored. */
    hype_pci_set_bar_size(&pci, HYPE_PCI_MAX_DEVICES, 0, 0x1000u);
    hype_pci_set_bar_size(&pci, 0, 6, 0x1000u);

    CHECK_HEX("bar 0 still unset", 0, hype_pci_get_bar_value(&pci, 0, 0));
}

static void test_get_bar_value_out_of_range_and_absent(void) {
    hype_pci_t pci;

    hype_pci_reset(&pci);

    CHECK_HEX("out-of-range device", 0, hype_pci_get_bar_value(&pci, HYPE_PCI_MAX_DEVICES, 0));
    CHECK_HEX("out-of-range bar index", 0, hype_pci_get_bar_value(&pci, 0, 6));
    CHECK_HEX("absent device", 0, hype_pci_get_bar_value(&pci, 0, 0));
}

static void test_memory_space_enabled_out_of_range_and_absent(void) {
    hype_pci_t pci;

    hype_pci_reset(&pci);

    CHECK_HEX("out-of-range device", 0, hype_pci_memory_space_enabled(&pci, HYPE_PCI_MAX_DEVICES));
    CHECK_HEX("absent device", 0, hype_pci_memory_space_enabled(&pci, 0));
}

static void test_bar_style_offset_with_non_4byte_access_uses_generic_path(void) {
    hype_pci_t pci;
    hype_pci_ecam_addr_t bar0_byte = {0, 0, 0, 0x10};
    uint32_t value;

    hype_pci_reset(&pci);
    hype_pci_add_device(&pci, 0, 0x1234, 0x5678, 6, 0, 0);
    hype_pci_set_bar_size(&pci, 0, 0, 0x1000u);

    /* A 1-byte access at a BAR offset isn't the sizing/programming
     * protocol (real firmware never does this) -- falls through to the
     * plain byte-buffer path, reading the BAR's current raw byte
     * (initially 0). */
    hype_pci_config_read(&pci, &bar0_byte, 1, &value);
    CHECK_HEX("1-byte access at a BAR offset reads raw byte", 0, value);

    hype_pci_config_write(&pci, &bar0_byte, 1, 0xFFu);
    hype_pci_config_read(&pci, &bar0_byte, 1, &value);
    CHECK_HEX("1-byte write at a BAR offset is a plain byte write", 0xFFu, value);
}

static void test_unaligned_bar_offset_uses_generic_path(void) {
    hype_pci_t pci;
    hype_pci_ecam_addr_t unaligned = {0, 0, 0, 0x11}; /* within 0x10-0x24 but not 4-aligned */
    uint32_t value;

    hype_pci_reset(&pci);
    hype_pci_add_device(&pci, 0, 0x1234, 0x5678, 6, 0, 0);
    hype_pci_set_bar_size(&pci, 0, 0, 0x1000u);

    hype_pci_config_write(&pci, &unaligned, 4, 0xFFFFFFFFu);
    hype_pci_config_read(&pci, &unaligned, 4, &value);
    /* Not the sizing protocol -- a plain 4-byte write/read at an
     * unaligned register, unaffected by BAR semantics. */
    CHECK_HEX("unaligned 4-byte access at a BAR-range offset", 0xFFFFFFFFu, value);
}

static void test_read_write_near_config_space_end_is_bounded(void) {
    hype_pci_t pci;
    hype_pci_ecam_addr_t near_end = {0, 0, 0, HYPE_PCI_CONFIG_SIZE - 2};
    uint32_t value;

    hype_pci_reset(&pci);
    hype_pci_add_device(&pci, 0, 0x1234, 0x5678, 6, 0, 0);

    /* A 4-byte access starting 2 bytes before the end of the 256-byte
     * config space must not write/read past it. */
    hype_pci_config_write(&pci, &near_end, 4, 0xAABBCCDDu);
    hype_pci_config_read(&pci, &near_end, 4, &value);
    CHECK_HEX("only the 2 in-bounds bytes were written", 0xCCDDu, value);
}

static void test_decode_cf8_address(void) {
    /* bus=0, device=0, function=0, register=0x08 (Revision ID/Class
     * Code dword) -- the register field is dword-aligned (bits 7:2
     * only), so 0x08 (not e.g. 0x02, an invalid unaligned value) ->
     * enable bit set + bus<<16 | device<<11 | function<<8 | register */
    hype_pci_ecam_addr_t addr;

    hype_pci_decode_cf8_address(0x80000008u, &addr);
    CHECK_HEX("bus", 0, addr.bus);
    CHECK_HEX("device", 0, addr.device);
    CHECK_HEX("function", 0, addr.function);
    CHECK_HEX("register", 0x08u, addr.register_offset);
}

static void test_decode_cf8_address_nonzero_bus_device_function(void) {
    /* bus=1, device=17 (0x11), function=3, register=0x0C -- same
     * fields as test_decode_ecam_offset(), different wire encoding. */
    hype_pci_ecam_addr_t addr;
    uint32_t cf8 = 0x80000000u | (1u << 16) | (17u << 11) | (3u << 8) | 0x0Cu;

    hype_pci_decode_cf8_address(cf8, &addr);
    CHECK_HEX("bus", 1, addr.bus);
    CHECK_HEX("device", 17, addr.device);
    CHECK_HEX("function", 3, addr.function);
    CHECK_HEX("register", 0x0Cu, addr.register_offset);
}

static void test_cf8_write_read_roundtrip(void) {
    hype_pci_t pci;

    hype_pci_reset(&pci);
    CHECK_HEX("cf8 starts at 0 after reset", 0, hype_pci_cf8_read(&pci));

    hype_pci_cf8_write(&pci, 0x80000123u);
    CHECK_HEX("cf8 readback", 0x80000123u, hype_pci_cf8_read(&pci));
}

static void test_cf8_config_read_write_via_selected_address(void) {
    hype_pci_t pci;
    uint32_t value;

    hype_pci_reset(&pci);
    hype_pci_add_device(&pci, 0, 0x8086, 0x29C0, 6, 0, 0);

    /* Select device 0's Vendor/Device ID register (0x00) and read it
     * back a whole dword at a time, exactly how OVMF's own
     * PlatformPei probes the host bridge. */
    hype_pci_cf8_write(&pci, 0x80000000u);
    hype_pci_cf8_config_read(&pci, 0, 4, &value);
    CHECK_HEX("vendor|device dword", 0x29C08086u, value);

    /* A narrower, byte-offset access through the CFD-equivalent port
     * (byte_offset=2) reads just the Device ID. */
    hype_pci_cf8_config_read(&pci, 2, 2, &value);
    CHECK_HEX("device id via byte_offset=2", 0x29C0u, value);
}

static void test_cf8_config_write_reaches_selected_device(void) {
    hype_pci_t pci;
    hype_pci_ecam_addr_t command_reg = {0, 0, 0, 0x04};
    uint32_t value;

    hype_pci_reset(&pci);
    hype_pci_add_device(&pci, 0, 0x8086, 0x29C0, 6, 0, 0);

    hype_pci_cf8_write(&pci, 0x80000004u); /* select the Command register */
    hype_pci_cf8_config_write(&pci, 0, 2, 0x0007u);

    hype_pci_config_read(&pci, &command_reg, 2, &value);
    CHECK_HEX("command register written via cf8/cfc", 0x0007u, value);
}

static void test_cf8_config_access_to_absent_device_reads_all_ones(void) {
    hype_pci_t pci;
    uint32_t value;

    hype_pci_reset(&pci);
    hype_pci_cf8_write(&pci, 0x80000000u | (5u << 11)); /* device 5, never registered */
    hype_pci_cf8_config_read(&pci, 0, 4, &value);
    CHECK_HEX("absent device via cf8/cfc reads all-1s", 0xFFFFFFFFu, value);
}

int main(void) {
    test_decode_ecam_offset();
    test_absent_device_reads_all_ones();
    test_non_bus_zero_or_non_function_zero_reads_absent();
    test_multifunction_device();
    test_add_device_populates_header();
    test_ahci_class_code();
    test_set_interrupt_pin_and_line();
    test_set_interrupt_ignores_out_of_range_device();
    test_msi_capability_and_guest_programming();
    test_bar_sizing_protocol();
    test_io_bar_sizing_protocol();
    test_ich9_ahci_pci_profile();
    test_unimplemented_bar_always_reads_zero();
    test_write_to_absent_device_is_dropped();
    test_memory_space_enable_bit();
    test_bus_master_enabled();
    test_bus_master_out_of_range_and_absent();
    test_add_device_out_of_range_rejected();
    test_set_bar_size_out_of_range_is_a_no_op();
    test_get_bar_value_out_of_range_and_absent();
    test_memory_space_enabled_out_of_range_and_absent();
    test_bar_style_offset_with_non_4byte_access_uses_generic_path();
    test_unaligned_bar_offset_uses_generic_path();
    test_read_write_near_config_space_end_is_bounded();
    test_decode_cf8_address();
    test_decode_cf8_address_nonzero_bus_device_function();
    test_cf8_write_read_roundtrip();
    test_cf8_config_read_write_via_selected_address();
    test_cf8_config_write_reaches_selected_device();
    test_cf8_config_access_to_absent_device_reads_all_ones();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
