#include <stdio.h>
#include "../../arch/x86_64/cpu/paging.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_encode_entry(void) {
    uint64_t e = hype_paging_encode_entry(0x0000123456789000ULL, HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE);
    CHECK_HEX("encode: address preserved", 0x0000123456789000ULL, e & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("encode: flags preserved", HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE, e & 0xFFFULL);
}

static void test_encode_entry_masks_low_bits_of_address(void) {
    /* An unaligned address must not leak its low 12 bits into the flags
     * region -- those bits belong to the flags field on a real entry,
     * so a caller passing a not-quite-aligned address must not corrupt
     * them. */
    uint64_t e = hype_paging_encode_entry(0x1000000000FFFULL, HYPE_PAGING_PRESENT);
    CHECK_HEX("encode: low 12 bits of address are masked off", 0, e & 0xFFFULL & ~HYPE_PAGING_PRESENT);
    CHECK_HEX("encode: PRESENT flag still set", HYPE_PAGING_PRESENT, e & HYPE_PAGING_PRESENT);
}

static void test_encode_entry_nx_bit(void) {
    uint64_t e = hype_paging_encode_entry(0, HYPE_PAGING_PRESENT | (1ULL << 63));
    CHECK_HEX("encode: NX bit (63) preserved", (1ULL << 63), e & (1ULL << 63));
}

static void test_encode_entry_flags_masked_to_allowed_bits(void) {
    /* Reserved bits (12-51 overlap with address, 52-62 always reserved)
     * must never leak in from a sloppy flags value. */
    uint64_t e = hype_paging_encode_entry(0, 0xFFFFFFFFFFFFFFFFULL);
    CHECK_HEX("encode: flags masked to bits 0-11 and 63 only", 0x8000000000000FFFULL, e);
}

static hype_pte_t g_pml4[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_pte_t g_pdpt[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_pte_t g_pd[4][HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));

static void test_build_identity(void) {
    unsigned int gb_to_map = 3;
    unsigned int i;

    hype_paging_build_identity(g_pml4, g_pdpt, g_pd, gb_to_map);

    CHECK_HEX("pml4[0] present+write", HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE, g_pml4[0] & 0xFFFULL);
    CHECK_HEX("pml4[0] points at pdpt", (uint64_t)g_pdpt, g_pml4[0] & 0x000FFFFFFFFFF000ULL);

    for (i = 1; i < HYPE_PAGING_ENTRIES_PER_TABLE; i++) {
        if (g_pml4[i] != 0) {
            printf("FAIL: pml4[%u] should be not-present (unused)\n", i);
            failures++;
            break;
        }
    }

    for (i = 0; i < gb_to_map; i++) {
        CHECK_HEX("pdpt[i] present+write", HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE, g_pdpt[i] & 0xFFFULL);
        CHECK_HEX("pdpt[i] points at pd_tables[i]", (uint64_t)g_pd[i], g_pdpt[i] & 0x000FFFFFFFFFF000ULL);
    }
    for (i = gb_to_map; i < HYPE_PAGING_ENTRIES_PER_TABLE; i++) {
        if (g_pdpt[i] != 0) {
            printf("FAIL: pdpt[%u] should be not-present (beyond gb_to_map)\n", i);
            failures++;
            break;
        }
    }

    /* Spot-check a handful of PD entries across GB boundaries. */
    CHECK_HEX("pd[0][0] maps physical 0", HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE | HYPE_PAGING_PS,
              g_pd[0][0] & 0xFFFULL);
    CHECK_HEX("pd[0][0] physical address", 0, g_pd[0][0] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("pd[0][511] physical address", 511ULL * HYPE_PAGING_2MB, g_pd[0][511] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("pd[1][0] physical address is 1GB", HYPE_PAGING_1GB, g_pd[1][0] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("pd[2][3] physical address", 2ULL * HYPE_PAGING_1GB + 3ULL * HYPE_PAGING_2MB,
              g_pd[2][3] & 0x000FFFFFFFFFF000ULL);
}

int main(void) {
    test_encode_entry();
    test_encode_entry_masks_low_bits_of_address();
    test_encode_entry_nx_bit();
    test_encode_entry_flags_masked_to_allowed_bits();
    test_build_identity();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
