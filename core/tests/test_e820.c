#include <stdio.h>
#include "../../devices/e820.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* Reassemble a little-endian value from the serialized blob so the test
 * asserts on the actual on-the-wire bytes OVMF will parse. */
static unsigned long long le(const unsigned char *p, unsigned int n) {
    unsigned long long v = 0;
    unsigned int i;
    for (i = 0; i < n; i++) {
        v |= (unsigned long long)p[i] << (i * 8);
    }
    return v;
}

static void test_single_ram_region(void) {
    unsigned char out[HYPE_E820_ENTRY_SIZE * 4];
    hype_e820_region_t r;
    int len;

    r.base = 0;
    r.length = 1024ULL * 1024ULL * 1024ULL; /* 1 GiB, FW-1a's guest RAM */
    r.type = HYPE_E820_TYPE_RAM;

    len = hype_e820_build(out, (uint32_t)sizeof(out), &r, 1);

    CHECK_HEX("returns one 20-byte entry", HYPE_E820_ENTRY_SIZE, len);
    CHECK_HEX("entry base little-endian", 0, le(out + 0, 8));
    CHECK_HEX("entry length little-endian", 1024ULL * 1024ULL * 1024ULL, le(out + 8, 8));
    CHECK_HEX("entry type = usable RAM", HYPE_E820_TYPE_RAM, le(out + 16, 4));
}

static void test_multiple_regions_packed_contiguously(void) {
    unsigned char out[HYPE_E820_ENTRY_SIZE * 4];
    hype_e820_region_t regs[2];
    int len;

    regs[0].base = 0;
    regs[0].length = 0x40000000ULL;
    regs[0].type = HYPE_E820_TYPE_RAM;
    regs[1].base = 0xE0000000ULL;
    regs[1].length = 0x10000000ULL;
    regs[1].type = HYPE_E820_TYPE_RESERVED;

    len = hype_e820_build(out, (uint32_t)sizeof(out), regs, 2);

    CHECK_HEX("returns two entries back to back", HYPE_E820_ENTRY_SIZE * 2, len);
    CHECK_HEX("entry[1] base at offset 20", 0xE0000000ULL, le(out + HYPE_E820_ENTRY_SIZE + 0, 8));
    CHECK_HEX("entry[1] length at offset 28", 0x10000000ULL, le(out + HYPE_E820_ENTRY_SIZE + 8, 8));
    CHECK_HEX("entry[1] type reserved at offset 36", HYPE_E820_TYPE_RESERVED,
              le(out + HYPE_E820_ENTRY_SIZE + 16, 4));
}

static void test_zero_count_rejected(void) {
    unsigned char out[HYPE_E820_ENTRY_SIZE];
    hype_e820_region_t r = {0, 0x1000, HYPE_E820_TYPE_RAM};
    CHECK_HEX("count 0 returns -1", (unsigned long long)(long long)-1,
              (unsigned long long)(long long)hype_e820_build(out, (uint32_t)sizeof(out), &r, 0));
}

static void test_capacity_too_small_rejected(void) {
    unsigned char out[HYPE_E820_ENTRY_SIZE]; /* room for one, ask for two */
    hype_e820_region_t regs[2] = {{0, 0x1000, HYPE_E820_TYPE_RAM}, {0x2000, 0x1000, HYPE_E820_TYPE_RAM}};
    CHECK_HEX("insufficient capacity returns -1", (unsigned long long)(long long)-1,
              (unsigned long long)(long long)hype_e820_build(out, (uint32_t)sizeof(out), regs, 2));
}

static void test_exact_capacity_accepted(void) {
    unsigned char out[HYPE_E820_ENTRY_SIZE]; /* exactly one entry */
    hype_e820_region_t r = {0, 0x1000, HYPE_E820_TYPE_RAM};
    CHECK_HEX("exact-fit capacity accepted", HYPE_E820_ENTRY_SIZE,
              hype_e820_build(out, (uint32_t)sizeof(out), &r, 1));
}

int main(void) {
    test_single_ram_region();
    test_multiple_regions_packed_contiguously();
    test_zero_count_rejected();
    test_capacity_too_small_rejected();
    test_exact_capacity_accepted();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
