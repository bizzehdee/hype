#include <stdio.h>
#include "../../arch/x86_64/cpu/gdt.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_encode_entry_null(void) {
    hype_gdt_entry_t e;
    hype_gdt_encode_entry(&e, 0, 0, 0, 0);
    CHECK_HEX("null limit_low", 0, e.limit_low);
    CHECK_HEX("null base_low", 0, e.base_low);
    CHECK_HEX("null base_mid", 0, e.base_mid);
    CHECK_HEX("null access", 0, e.access);
    CHECK_HEX("null limit_high_flags", 0, e.limit_high_flags);
    CHECK_HEX("null base_high", 0, e.base_high);
}

static void test_encode_entry_code64(void) {
    hype_gdt_entry_t e;
    /* Conventional flat 64-bit code descriptor: base=0, limit=0xFFFFF,
     * access=0x9A, flags nibble=0xA (G=1,D=0,L=1,AVL=0). Known-good byte
     * values (0x00 0x00 0x00 0x9A 0xAF 0x00) match what every OS-dev
     * long-mode GDT reference uses for this exact descriptor. */
    hype_gdt_encode_entry(&e, 0, 0xFFFFF, 0x9A, 0xA);
    CHECK_HEX("code64 limit_low", 0xFFFF, e.limit_low);
    CHECK_HEX("code64 base_low", 0x0000, e.base_low);
    CHECK_HEX("code64 base_mid", 0x00, e.base_mid);
    CHECK_HEX("code64 access", 0x9A, e.access);
    CHECK_HEX("code64 limit_high_flags", 0xAF, e.limit_high_flags);
    CHECK_HEX("code64 base_high", 0x00, e.base_high);
}

static void test_encode_entry_data(void) {
    hype_gdt_entry_t e;
    hype_gdt_encode_entry(&e, 0, 0xFFFFF, 0x92, 0xC);
    CHECK_HEX("data access", 0x92, e.access);
    CHECK_HEX("data limit_high_flags", 0xCF, e.limit_high_flags);
}

static void test_encode_entry_nonzero_base(void) {
    hype_gdt_entry_t e;
    /* base=0x12345678, limit=0xABCDE -- exercises every byte split point
     * (base_low/mid/high, limit_low/high) with non-trivial values.
     * limit's low 16 bits are 0xBCDE; the top hex digit (0xA) is the
     * only part that lands in limit_high_flags' low nibble. */
    hype_gdt_encode_entry(&e, 0x12345678u, 0xABCDEu, 0x00, 0x00);
    CHECK_HEX("nonzero base_low", 0x5678, e.base_low);
    CHECK_HEX("nonzero base_mid", 0x34, e.base_mid);
    CHECK_HEX("nonzero base_high", 0x12, e.base_high);
    CHECK_HEX("nonzero limit_low", 0xBCDE, e.limit_low);
    CHECK_HEX("nonzero limit_high nibble", 0x0A, e.limit_high_flags);
}

static void test_encode_entry_flags_masked_to_nibble(void) {
    hype_gdt_entry_t e;
    /* flags is only ever a 4-bit field -- passing garbage (0xFF) in it
     * must be masked to its low nibble (0xF) before landing in the
     * upper nibble of limit_high_flags, not corrupt/replace the whole
     * byte: limit=0xF contributes 0x0 to the low nibble (0xF >> 16 ==
     * 0), flags contributes 0xF0, so the result is 0xF0, not 0xFF. */
    hype_gdt_encode_entry(&e, 0, 0xF, 0x00, 0xFF);
    CHECK_HEX("flags masked to low nibble before shifting", 0xF0, e.limit_high_flags);
}

static void test_build(void) {
    hype_gdt_entry_t table[HYPE_GDT_ENTRY_COUNT];

    hype_gdt_build(table);

    CHECK_HEX("build: null descriptor is all zero (access)", 0, table[0].access);
    CHECK_HEX("build: null descriptor is all zero (limit_high_flags)", 0, table[0].limit_high_flags);

    CHECK_HEX("build: code64 access byte", 0x9A, table[1].access);
    CHECK_HEX("build: code64 limit_high_flags (G|L, limit=0xFFFFF)", 0xAF, table[1].limit_high_flags);
    CHECK_HEX("build: code64 limit_low", 0xFFFF, table[1].limit_low);

    CHECK_HEX("build: data access byte", 0x92, table[2].access);
    CHECK_HEX("build: data limit_high_flags (G|D, limit=0xFFFFF)", 0xCF, table[2].limit_high_flags);
}

int main(void) {
    test_encode_entry_null();
    test_encode_entry_code64();
    test_encode_entry_data();
    test_encode_entry_nonzero_base();
    test_encode_entry_flags_masked_to_nibble();
    test_build();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
