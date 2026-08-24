#include <stdint.h>
#include <stdio.h>

#include "../avic.h"

/* #193: unit tests for the pure AVIC table logic. */

static int failures = 0;
#define CHECK_INT(desc, expected, actual)                                                          \
    do {                                                                                           \
        long long _e = (long long)(expected), _a = (long long)(actual);                            \
        if (_e != _a) { printf("FAIL: %s: expected %lld, got %lld\n", (desc), _e, _a); failures++; } \
    } while (0)
#define CHECK_HEX(desc, expected, actual)                                                          \
    do {                                                                                           \
        unsigned long long _e = (unsigned long long)(expected), _a = (unsigned long long)(actual); \
        if (_e != _a) { printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), _e, _a); failures++; } \
    } while (0)

static void test_capability_decode(void) {
    CHECK_INT("AVIC bit 13 set -> supported", 1, hype_avic_supported(1u << 13));
    CHECK_INT("AVIC bit clear -> unsupported", 0, hype_avic_supported(0));
    /* other SVM feature bits do not imply AVIC */
    CHECK_INT("pause-filter bit alone -> unsupported", 0, hype_avic_supported(1u << 10));
    CHECK_INT("AVIC among others -> supported", 1, hype_avic_supported((1u << 10) | (1u << 13)));
}

static void test_physical_entry(void) {
    uint64_t e = hype_avic_physical_entry(0x5u, 0x123456000ull, 1 /* running */, 1 /* valid */);
    CHECK_HEX("host apic id in [11:0]", 0x5u, e & 0xFFFull);
    CHECK_HEX("backing page in [51:12]", 0x123456000ull, e & 0x000FFFFFFFFFF000ull);
    CHECK_INT("valid bit 63", 1, (e >> 63) & 1u);
    CHECK_INT("is-running bit 62", 1, (e >> 62) & 1u);
    /* not running */
    e = hype_avic_physical_entry(0x7u, 0x9000ull, 0, 1);
    CHECK_INT("is-running clear", 0, (e >> 62) & 1u);
    CHECK_INT("still valid", 1, (e >> 63) & 1u);
    /* invalid -> zero */
    CHECK_HEX("invalid entry is zero", 0, hype_avic_physical_entry(0x7u, 0x9000ull, 1, 0));
    /* the backing page's low 12 bits are ignored (page-aligned field) */
    e = hype_avic_physical_entry(0x1u, 0x123456FFFull, 0, 1);
    CHECK_HEX("backing page masked to [51:12]", 0x123456000ull, e & 0x000FFFFFFFFFF000ull);
}

static void test_logical_entry(void) {
    uint32_t e = hype_avic_logical_entry(0x3u, 1);
    CHECK_HEX("guest phys id in [7:0]", 0x3u, e & 0xFFu);
    CHECK_INT("valid bit 31", 1, (e >> 31) & 1u);
    CHECK_HEX("invalid -> zero", 0, hype_avic_logical_entry(0x3u, 0));
}

static void test_build_physical_table(void) {
    static uint64_t table[512];
    uint32_t host_ids[3] = {0u, 2u, 4u};
    uint64_t backing[3] = {0x100000000ull, 0x100001000ull, 0x100002000ull};
    uint8_t maxidx = 0xFFu;
    unsigned int n = hype_avic_build_physical_table(table, 512u, host_ids, backing, 3u, &maxidx);
    CHECK_INT("built 3 entries", 3, n);
    CHECK_INT("max index = 2", 2, maxidx);
    CHECK_HEX("entry0 host id", 0u, table[0] & 0xFFFull);
    CHECK_HEX("entry1 host id", 2u, table[1] & 0xFFFull);
    CHECK_HEX("entry1 backing", 0x100001000ull, table[1] & 0x000FFFFFFFFFF000ull);
    CHECK_INT("entry2 valid", 1, (table[2] >> 63) & 1u);
    CHECK_HEX("entry3 cleared", 0, table[3]);
    /* count 0 -> max index 0, nothing valid */
    maxidx = 0xFFu;
    n = hype_avic_build_physical_table(table, 512u, host_ids, backing, 0u, &maxidx);
    CHECK_INT("count 0 builds nothing", 0, n);
    CHECK_INT("max index 0 when empty", 0, maxidx);
    CHECK_HEX("entry0 cleared on empty build", 0, table[0]);
    /* count clamps to table_entries */
    n = hype_avic_build_physical_table(table, 2u, host_ids, backing, 3u, &maxidx);
    CHECK_INT("count clamped to table size", 2, n);
}

static void test_bitmap_highest(void) {
    uint32_t words[8];
    unsigned int i;
    for (i = 0; i < 8u; i++) words[i] = 0u;
    CHECK_INT("empty bitmap -> -1", -1, hype_avic_bitmap_highest(words));
    words[0] = 1u; /* vector 0 */
    CHECK_INT("only vector 0 set", 0, hype_avic_bitmap_highest(words));
    words[3] = 1u << 5; /* vector 3*32+5 = 101 */
    CHECK_INT("higher word wins over lower", 101, hype_avic_bitmap_highest(words));
    words[7] = 1u << 31; /* vector 255, the highest possible */
    CHECK_INT("top word top bit -> 255", 255, hype_avic_bitmap_highest(words));
    words[7] |= 1u << 3; /* an even lower bit in the SAME top word */
    CHECK_INT("highest bit within the top word wins", 255, hype_avic_bitmap_highest(words));
}

static void test_ldr_flat_index(void) {
    CHECK_INT("logical id 0x01 -> index 0", 0, hype_avic_ldr_flat_index(0x01000000u));
    CHECK_INT("logical id 0x80 -> index 7", 7, hype_avic_ldr_flat_index(0x80000000u));
    CHECK_INT("logical id 0x10 -> index 4", 4, hype_avic_ldr_flat_index(0x10000000u));
    CHECK_INT("all-zero logical id -> -1", -1, hype_avic_ldr_flat_index(0u));
    CHECK_INT("multi-bit logical id (cluster-mode-shaped) -> -1", -1,
              hype_avic_ldr_flat_index(0x03000000u));
    /* low 24 bits (reserved in the register, or garbage from a sloppy write) never matter */
    CHECK_INT("low bits ignored", 0, hype_avic_ldr_flat_index(0x010000FFu));
}

int main(void) {
    test_capability_decode();
    test_physical_entry();
    test_logical_entry();
    test_build_physical_table();
    test_bitmap_highest();
    test_ldr_flat_index();
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
