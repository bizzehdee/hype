#include <stdio.h>
#include "../../devices/cmos.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_reset_is_all_zero(void) {
    hype_cmos_t cmos;
    hype_cmos_reset(&cmos);
    CHECK_HEX("index starts at 0", 0, cmos.index);
    CHECK_HEX("register 0 starts at 0", 0, cmos.registers[0]);
    CHECK_HEX("register 0x34 starts at 0", 0, cmos.registers[HYPE_CMOS_REG_EXTMEM_LOW]);
}

static void test_index_write_masks_nmi_disable_bit(void) {
    hype_cmos_t cmos;
    hype_cmos_reset(&cmos);
    hype_cmos_index_write(&cmos, 0x80u | HYPE_CMOS_REG_EXTMEM_LOW);
    CHECK_HEX("bit 7 (NMI-disable) masked off the stored index", HYPE_CMOS_REG_EXTMEM_LOW, cmos.index);
}

static void test_data_read_write_roundtrip(void) {
    hype_cmos_t cmos;
    hype_cmos_reset(&cmos);
    hype_cmos_index_write(&cmos, 0x10u);
    hype_cmos_data_write(&cmos, 0xABu);
    CHECK_HEX("data written reads back", 0xABu, hype_cmos_data_read(&cmos));

    hype_cmos_index_write(&cmos, 0x11u);
    CHECK_HEX("a different register is independent", 0, hype_cmos_data_read(&cmos));
}

static void test_set_extended_memory_above_16mb(void) {
    hype_cmos_t cmos;
    hype_cmos_reset(&cmos);

    /* 0x1234 in 64KB units -- low byte 0x34, high byte 0x12. */
    hype_cmos_set_extended_memory_above_16mb(&cmos, 0x1234u);

    hype_cmos_index_write(&cmos, HYPE_CMOS_REG_EXTMEM_LOW);
    CHECK_HEX("extmem low byte", 0x34u, hype_cmos_data_read(&cmos));

    hype_cmos_index_write(&cmos, HYPE_CMOS_REG_EXTMEM_HIGH);
    CHECK_HEX("extmem high byte", 0x12u, hype_cmos_data_read(&cmos));
}

static void test_index_out_of_bounds_wraps_within_register_file(void) {
    hype_cmos_t cmos;
    hype_cmos_reset(&cmos);

    /* 0xFF & 0x7F = 0x7F, the last valid register -- never out of
     * bounds regardless of what's written to port 0x70. */
    hype_cmos_index_write(&cmos, 0xFFu);
    CHECK_HEX("index clamped to the last valid register", 0x7Fu, cmos.index);
    hype_cmos_data_write(&cmos, 0x42u);
    CHECK_HEX("last register readable", 0x42u, hype_cmos_data_read(&cmos));
}

int main(void) {
    test_reset_is_all_zero();
    test_index_write_masks_nmi_disable_bit();
    test_data_read_write_roundtrip();
    test_set_extended_memory_above_16mb();
    test_index_out_of_bounds_wraps_within_register_file();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
