#include <stdio.h>
#include "../../arch/x86_64/svm/npt.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static hype_pte_t g_pml4[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_pte_t g_pdpt[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_pte_t g_pd[3][HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));

static void test_build_identity(void) {
    unsigned int gb_to_map = 3;
    unsigned int i;
    uint64_t expected_table_flags = HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE | HYPE_PAGING_USER;
    uint64_t expected_page_flags = expected_table_flags | HYPE_PAGING_PS;

    hype_npt_build_identity(g_pml4, g_pdpt, g_pd, gb_to_map);

    CHECK_HEX("pml4[0] present+write+user", expected_table_flags, g_pml4[0] & 0xFFFULL);
    CHECK_HEX("pml4[0] points at pdpt", (uint64_t)g_pdpt, g_pml4[0] & 0x000FFFFFFFFFF000ULL);

    for (i = 1; i < HYPE_PAGING_ENTRIES_PER_TABLE; i++) {
        if (g_pml4[i] != 0) {
            printf("FAIL: pml4[%u] should be not-present (unused)\n", i);
            failures++;
            break;
        }
    }

    for (i = 0; i < gb_to_map; i++) {
        CHECK_HEX("pdpt[i] present+write+user", expected_table_flags, g_pdpt[i] & 0xFFFULL);
        CHECK_HEX("pdpt[i] points at pd_tables[i]", (uint64_t)g_pd[i], g_pdpt[i] & 0x000FFFFFFFFFF000ULL);
    }
    for (i = gb_to_map; i < HYPE_PAGING_ENTRIES_PER_TABLE; i++) {
        if (g_pdpt[i] != 0) {
            printf("FAIL: pdpt[%u] should be not-present (beyond gb_to_map)\n", i);
            failures++;
            break;
        }
    }

    CHECK_HEX("pd[0][0] present+write+user+ps", expected_page_flags, g_pd[0][0] & 0xFFFULL);
    CHECK_HEX("pd[0][0] maps guest-physical 0 to host-physical 0", 0,
              g_pd[0][0] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("pd[0][511] physical address", 511ULL * HYPE_PAGING_2MB, g_pd[0][511] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("pd[1][0] physical address is 1GB", HYPE_PAGING_1GB, g_pd[1][0] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("pd[2][3] physical address", 2ULL * HYPE_PAGING_1GB + 3ULL * HYPE_PAGING_2MB,
              g_pd[2][3] & 0x000FFFFFFFFFF000ULL);
}

int main(void) {
    test_build_identity();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
