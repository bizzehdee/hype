#include <stdio.h>
#include "../host_pci.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* --- Fake config spaces (one read32 fn per topology) --- */

/* Single-function AHCI controller at 00:1f.0 (class 01/06/01, 32-bit ABAR). */
static uint32_t cfg_ahci(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    if (bus != 0 || dev != 31 || func != 0) {
        return 0xFFFFFFFFu; /* every other slot: no device */
    }
    switch (off) {
        case 0x00: return 0x29228086u;           /* vendor 8086, device 2922 */
        case 0x08: return 0x01060102u;           /* class 01, subclass 06 (AHCI), prog-if 01 */
        case 0x0C: return 0x00000000u;           /* header type 0, single function */
        case 0x24: return 0xFEBF1000u;           /* BAR5 (ABAR): 32-bit mem, base 0xFEBF1000 */
        default:   return 0x00000000u;
    }
}

/* NVMe controller at 00:01.0 (class 01/08/02) with a 64-bit BAR0. */
static uint32_t cfg_nvme(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    if (bus != 0 || dev != 1 || func != 0) {
        return 0xFFFFFFFFu;
    }
    switch (off) {
        case 0x00: return 0x00111d1eu;           /* some vendor/device */
        case 0x08: return 0x01080200u;           /* class 01, subclass 08 (NVMe), prog-if 02 */
        case 0x0C: return 0x00000000u;
        case 0x10: return 0xF0000004u;           /* BAR0 lo: 64-bit mem (bits[2:1]=10), base low 0xF0000000 */
        case 0x14: return 0x00000001u;           /* BAR0 hi: base high */
        default:   return 0x00000000u;
    }
}

/* No storage anywhere: a lone display controller at 00:02.0 (class 03). */
static uint32_t cfg_no_storage(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    if (bus != 0 || dev != 2 || func != 0) {
        return 0xFFFFFFFFu;
    }
    switch (off) {
        case 0x00: return 0x12345678u;
        case 0x08: return 0x03000000u;           /* class 03 (display), not storage */
        case 0x0C: return 0x00000000u;
        default:   return 0x00000000u;
    }
}

/* Multi-function device at 00:1f: func0 is an LPC bridge (class 06) with the MF
 * bit set; func2 is the AHCI controller. Exercises the MF-probe path. */
static uint32_t cfg_multifunction(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    if (bus != 0 || dev != 31) {
        return 0xFFFFFFFFu;
    }
    if (func == 0) {
        switch (off) {
            case 0x00: return 0x1c008086u;
            case 0x08: return 0x06010000u;       /* class 06 (bridge) -- not storage */
            case 0x0C: return 0x00800000u;       /* header-type byte (bits[23:16]) = 0x80: MF bit set */
            default:   return 0x00000000u;
        }
    }
    if (func == 2) {
        switch (off) {
            case 0x00: return 0x29228086u;
            case 0x08: return 0x01060102u;       /* AHCI */
            case 0x0C: return 0x00000000u;
            case 0x24: return 0xFEBF1000u;       /* ABAR */
            default:   return 0x00000000u;
        }
    }
    return 0xFFFFFFFFu; /* func1, func3..7 absent */
}

static void test_find_ahci(void) {
    hype_host_storage_t st;
    int found = hype_host_pci_find_storage(cfg_ahci, 0, &st);
    CHECK_HEX("AHCI found", 1, found);
    CHECK_HEX("kind = AHCI", HYPE_HOST_STORAGE_AHCI, st.kind);
    CHECK_HEX("bus", 0, st.bus);
    CHECK_HEX("dev", 31, st.dev);
    CHECK_HEX("func", 0, st.func);
    CHECK_HEX("vendor", 0x8086, st.vendor_id);
    CHECK_HEX("ABAR base (low bits masked)", 0xFEBF1000ull, st.bar_phys);
}

static void test_find_nvme_64bit_bar(void) {
    hype_host_storage_t st;
    int found = hype_host_pci_find_storage(cfg_nvme, 0, &st);
    CHECK_HEX("NVMe found", 1, found);
    CHECK_HEX("kind = NVMe", HYPE_HOST_STORAGE_NVME, st.kind);
    CHECK_HEX("64-bit BAR0 base assembled from both dwords", 0x1F0000000ull, st.bar_phys);
}

static void test_no_storage(void) {
    hype_host_storage_t st;
    int found = hype_host_pci_find_storage(cfg_no_storage, 0, &st);
    CHECK_HEX("nothing found", 0, found);
    CHECK_HEX("kind = NONE", HYPE_HOST_STORAGE_NONE, st.kind);
}

static void test_multifunction_storage(void) {
    hype_host_storage_t st;
    int found = hype_host_pci_find_storage(cfg_multifunction, 0, &st);
    CHECK_HEX("AHCI behind a multifunction device is found", 1, found);
    CHECK_HEX("kind = AHCI", HYPE_HOST_STORAGE_AHCI, st.kind);
    CHECK_HEX("func = 2", 2, st.func);
    CHECK_HEX("ABAR base", 0xFEBF1000ull, st.bar_phys);
}

int main(void) {
    test_find_ahci();
    test_find_nvme_64bit_bar();
    test_no_storage();
    test_multifunction_storage();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
