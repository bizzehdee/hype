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

static void test_mark_not_present(void) {
    hype_npt_build_identity(g_pml4, g_pdpt, g_pd, 3);

    hype_npt_mark_not_present(g_pd, 2ULL * HYPE_PAGING_1GB + 3ULL * HYPE_PAGING_2MB);

    CHECK_HEX("marked entry is fully cleared (not-present)", 0, g_pd[2][3]);
    CHECK_HEX("neighboring entry within the same PD is untouched", 1,
              g_pd[2][2] != 0);
    CHECK_HEX("neighboring entry within the same PD is untouched (after)", 1,
              g_pd[2][4] != 0);
}

static void test_map_range_single_entry(void) {
    hype_npt_build_identity(g_pml4, g_pdpt, g_pd, 3);

    /* Remap guest-physical 1GB (gb=1, pd index 0) to a completely
     * different host-physical address -- e.g. the classic "top of
     * 4GB" range this project's own build_identity sweep would
     * otherwise identity-map onto real host firmware flash instead of
     * available RAM (FW-1). */
    hype_npt_map_range(g_pd, HYPE_PAGING_1GB, 0xDEAD000000ULL, HYPE_PAGING_2MB);

    CHECK_HEX("remapped entry points at the new host address", 0xDEAD000000ULL,
              g_pd[1][0] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("remapped entry still present+write+user+ps",
              HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE | HYPE_PAGING_USER | HYPE_PAGING_PS,
              g_pd[1][0] & 0xFFFULL);
    CHECK_HEX("neighboring entry (pd[1][1]) untouched -- still identity",
              HYPE_PAGING_1GB + HYPE_PAGING_2MB, g_pd[1][1] & 0x000FFFFFFFFFF000ULL);
}

static void test_map_range_spans_multiple_entries_and_gb_boundary(void) {
    hype_npt_build_identity(g_pml4, g_pdpt, g_pd, 3);

    /* Starts at the last 2MB entry of gb=0 and spans into gb=1 --
     * exercises the gb/pd_index recomputation crossing a 1GB
     * boundary, matching FW-1's own real address layout (guest-
     * physical 0xFFC00000, within gb=3, spanning its last two 2MB
     * entries). */
    hype_npt_map_range(g_pd, 511ULL * HYPE_PAGING_2MB, 0x9000000000ULL, 2ULL * HYPE_PAGING_2MB);

    CHECK_HEX("first remapped entry (pd[0][511])", 0x9000000000ULL,
              g_pd[0][511] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("second remapped entry (pd[1][0])", 0x9000000000ULL + HYPE_PAGING_2MB,
              g_pd[1][0] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("entry before the remapped range (pd[0][510]) untouched -- still identity",
              510ULL * HYPE_PAGING_2MB, g_pd[0][510] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("entry after the remapped range (pd[1][1]) untouched -- still identity",
              HYPE_PAGING_1GB + HYPE_PAGING_2MB, g_pd[1][1] & 0x000FFFFFFFFFF000ULL);
}

static void test_mark_range_not_present_spans_gb_boundary(void) {
    hype_npt_build_identity(g_pml4, g_pdpt, g_pd, 3);

    /* FW-1a: clear guest-physical [0.5GB, 1.5GB) -- 512 * 2MB entries
     * spanning the gb=0 -> gb=1 boundary -- the "punch out everything
     * between guest RAM and the flash window" step. */
    hype_npt_mark_range_not_present(g_pd, HYPE_PAGING_1GB / 2, HYPE_PAGING_1GB);

    CHECK_HEX("entry just below the cleared range (pd[0][255]) untouched",
              255ULL * HYPE_PAGING_2MB, g_pd[0][255] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("first cleared entry (pd[0][256])", 0, g_pd[0][256]);
    CHECK_HEX("last entry of gb=0 cleared (pd[0][511])", 0, g_pd[0][511]);
    CHECK_HEX("first entry of gb=1 cleared (pd[1][0])", 0, g_pd[1][0]);
    CHECK_HEX("last cleared entry (pd[1][255])", 0, g_pd[1][255]);
    CHECK_HEX("entry just above the cleared range (pd[1][256]) untouched -- still identity",
              HYPE_PAGING_1GB + 256ULL * HYPE_PAGING_2MB, g_pd[1][256] & 0x000FFFFFFFFFF000ULL);
}

int main(void) {
    test_build_identity();
    test_mark_not_present();
    test_mark_range_not_present_spans_gb_boundary();
    test_map_range_single_entry();
    test_map_range_spans_multiple_entries_and_gb_boundary();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
