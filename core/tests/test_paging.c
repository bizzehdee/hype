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

static hype_pte_t g_fb_pd[2][HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));

static void test_map_region_high_framebuffer(void) {
    /* An Intel i5-13420H places the GOP framebuffer BAR at 256GB. */
    uint64_t fb_base = 0x4000000000ULL; /* 256 GB, 1GB-aligned */
    uint64_t fb_size = 0x7e9000ULL;     /* ~8.3 MB (from the real screen dump) */
    unsigned int mapped;
    uint64_t gb_index = fb_base / HYPE_PAGING_1GB; /* 256 */

    hype_paging_build_identity(g_pml4, g_pdpt, g_pd, 4);
    /* The BAR's PDPT slot starts not-present after the low identity map. */
    CHECK_HEX("pdpt[256] absent before mapping", 0, g_pdpt[gb_index]);

    mapped = hype_paging_map_region_2mb(g_pdpt, g_fb_pd, fb_base, fb_size);
    CHECK_HEX("one GB slot mapped (fb fits in one GB)", 1, mapped);
    CHECK_HEX("pdpt[256] now present+write", HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE,
              g_pdpt[gb_index] & 0xFFFULL);
    CHECK_HEX("pdpt[256] points at fb pd", (uint64_t)g_fb_pd[0],
              g_pdpt[gb_index] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("fb pd[0] identity-maps 256GB as a 2MB page",
              HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE | HYPE_PAGING_PS, g_fb_pd[0][0] & 0xFFFULL);
    CHECK_HEX("fb pd[0] physical address is 256GB", fb_base, g_fb_pd[0][0] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("fb pd[1] physical address is 256GB+2MB", fb_base + HYPE_PAGING_2MB,
              g_fb_pd[0][1] & 0x000FFFFFFFFFF000ULL);
    /* The low identity map is untouched. */
    CHECK_HEX("low map pdpt[0] still present", HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE,
              g_pdpt[0] & 0xFFFULL);
}

static void test_map_region_straddling_gb_boundary(void) {
    /* A region starting 1MB below a 1GB boundary spans two GB slots. */
    uint64_t base = 8ULL * HYPE_PAGING_1GB - 0x100000ULL;
    unsigned int mapped;
    hype_paging_build_identity(g_pml4, g_pdpt, g_pd, 4);
    mapped = hype_paging_map_region_2mb(g_pdpt, g_fb_pd, base, 0x400000ULL /* 4MB */);
    CHECK_HEX("straddling region maps two GB slots", 2, mapped);
    CHECK_HEX("pdpt[7] present", HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE, g_pdpt[7] & 0xFFFULL);
    CHECK_HEX("pdpt[8] present", HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE, g_pdpt[8] & 0xFFFULL);
}

static void test_map_region_out_of_range_and_empty(void) {
    hype_paging_build_identity(g_pml4, g_pdpt, g_pd, 4);
    /* Beyond PML4[0] (>= 512GB) -> refused. */
    CHECK_HEX("region at/above 512GB refused", 0,
              hype_paging_map_region_2mb(g_pdpt, g_fb_pd, 512ULL * HYPE_PAGING_1GB, 0x1000ULL));
    /* Zero size -> nothing mapped. */
    CHECK_HEX("zero-size region maps nothing", 0,
              hype_paging_map_region_2mb(g_pdpt, g_fb_pd, 0x4000000000ULL, 0));
}

static hype_pte_t g_mmio_pdpt[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_pte_t g_mmio_pd[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));

static void test_map_mmio_1gb_high(void) {
    /* An NVMe BAR QEMU placed at ~56 TiB (0x380000000000) -- above PML4[0]. */
    uint64_t bar = 0x380000000000ULL;
    uint64_t gb = bar / HYPE_PAGING_1GB; /* 57344 */
    unsigned int idx;

    hype_paging_build_identity(g_pml4, g_pdpt, g_pd, 4);
    idx = hype_paging_map_mmio_1gb(g_pml4, g_mmio_pdpt, g_mmio_pd, bar);
    CHECK_HEX("pml4 index = 57344/512 = 112", 112u, idx);
    CHECK_HEX("pml4[112] present+write", HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE,
              g_pml4[112] & 0xFFFULL);
    CHECK_HEX("pml4[112] -> mmio pdpt", (uint64_t)g_mmio_pdpt,
              g_pml4[112] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("pdpt[0] present -> mmio pd", (uint64_t)g_mmio_pd,
              g_mmio_pdpt[gb % HYPE_PAGING_ENTRIES_PER_TABLE] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("pd[0] present+write+PS+PCD (uncacheable MMIO)",
              HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE | HYPE_PAGING_PS | HYPE_PAGING_PCD,
              g_mmio_pd[0] & 0xFFFULL);
    CHECK_HEX("pd[0] maps the BAR's 1GB base", bar, g_mmio_pd[0] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("pd[1] = base + 2MB", bar + HYPE_PAGING_2MB, g_mmio_pd[1] & 0x000FFFFFFFFFF000ULL);
    /* PML4[0] low identity map untouched. */
    CHECK_HEX("pml4[0] still present", HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE, g_pml4[0] & 0xFFFULL);
}

int main(void) {
    test_encode_entry();
    test_map_mmio_1gb_high();
    test_encode_entry_masks_low_bits_of_address();
    test_encode_entry_nx_bit();
    test_encode_entry_flags_masked_to_allowed_bits();
    test_build_identity();
    test_map_region_high_framebuffer();
    test_map_region_straddling_gb_boundary();
    test_map_region_out_of_range_and_empty();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
