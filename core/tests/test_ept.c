#include <stdio.h>
#include "../../arch/x86_64/cpu/paging.h"
#include "../../arch/x86_64/vmx/ept.h"

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
    uint64_t e = hype_ept_encode_entry(0x0000123456789000ULL, HYPE_EPT_READ | HYPE_EPT_WRITE);
    CHECK_HEX("encode: address preserved", 0x0000123456789000ULL, e & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("encode: flags preserved", HYPE_EPT_READ | HYPE_EPT_WRITE, e & 0xFFULL);
}

static void test_encode_entry_masks_low_bits_of_address(void) {
    uint64_t e = hype_ept_encode_entry(0x1000000000FFFULL, HYPE_EPT_READ);
    CHECK_HEX("encode: low 12 bits of address are masked off", 0, e & 0xFFFULL & ~HYPE_EPT_READ);
    CHECK_HEX("encode: READ flag still set", HYPE_EPT_READ, e & HYPE_EPT_READ);
}

static void test_encode_entry_flags_masked_to_allowed_bits(void) {
    uint64_t e = hype_ept_encode_entry(0, 0xFFFFFFFFFFFFFFFFULL);
    CHECK_HEX("encode: flags masked to bits 0-7 only", 0xFFULL, e);
}

static hype_ept_pte_t g_pml4[HYPE_EPT_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_ept_pte_t g_pdpt[HYPE_EPT_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_ept_pte_t g_pd[3][HYPE_EPT_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));

static void test_build_identity(void) {
    unsigned int gb_to_map = 3;
    unsigned int i;
    uint64_t expected_table_flags = HYPE_EPT_READ | HYPE_EPT_WRITE | HYPE_EPT_EXEC;
    uint64_t expected_page_flags = expected_table_flags | HYPE_EPT_MEMTYPE_WB | HYPE_EPT_PS;

    hype_ept_build_identity(g_pml4, g_pdpt, g_pd, gb_to_map);

    CHECK_HEX("pml4[0] R/W/X", expected_table_flags, g_pml4[0] & 0xFFULL);
    CHECK_HEX("pml4[0] points at pdpt", (uint64_t)g_pdpt, g_pml4[0] & 0x000FFFFFFFFFF000ULL);

    for (i = 1; i < HYPE_EPT_ENTRIES_PER_TABLE; i++) {
        if (g_pml4[i] != 0) {
            printf("FAIL: pml4[%u] should be not-present (unused)\n", i);
            failures++;
            break;
        }
    }

    for (i = 0; i < gb_to_map; i++) {
        CHECK_HEX("pdpt[i] R/W/X", expected_table_flags, g_pdpt[i] & 0xFFULL);
        CHECK_HEX("pdpt[i] points at pd_tables[i]", (uint64_t)g_pd[i], g_pdpt[i] & 0x000FFFFFFFFFF000ULL);
    }
    for (i = gb_to_map; i < HYPE_EPT_ENTRIES_PER_TABLE; i++) {
        if (g_pdpt[i] != 0) {
            printf("FAIL: pdpt[%u] should be not-present (beyond gb_to_map)\n", i);
            failures++;
            break;
        }
    }

    CHECK_HEX("pd[0][0] R/W/X + WB memtype + PS", expected_page_flags, g_pd[0][0] & 0xFFULL);
    CHECK_HEX("pd[0][0] maps guest-physical 0 to host-physical 0", 0,
              g_pd[0][0] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("pd[0][511] physical address", 511ULL * HYPE_PAGING_2MB, g_pd[0][511] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("pd[1][0] physical address is 1GB", HYPE_PAGING_1GB, g_pd[1][0] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("pd[2][3] physical address", 2ULL * HYPE_PAGING_1GB + 3ULL * HYPE_PAGING_2MB,
              g_pd[2][3] & 0x000FFFFFFFFFF000ULL);
}

int main(void) {
    test_encode_entry();
    test_encode_entry_masks_low_bits_of_address();
    test_encode_entry_flags_masked_to_allowed_bits();
    test_build_identity();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
