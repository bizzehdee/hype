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

/*
 * #240: the real-hardware case the two older helpers left uncovered -- a BAR
 * above the low identity map but INSIDE PML4[0]. The whole point is that the
 * surrounding low map survives, which is what map_mmio_1gb would have destroyed
 * by overwriting pml4[0].
 */
static void test_map_mmio_1gb_into_pdpt_preserves_low_map(void) {
    /* The Intel i5-13420H's xHCI BAR: 0x6001120000 = 384 GiB. */
    uint64_t bar = 0x6001120000ULL;
    uint64_t gb = bar / HYPE_PAGING_1GB; /* 384 */
    uint64_t gb_base = gb * HYPE_PAGING_1GB;
    unsigned int idx;

    hype_paging_build_identity(g_pml4, g_pdpt, g_pd, 4);
    CHECK_HEX("precondition: the BAR's GB slot starts out absent", 0u, g_pdpt[384] & 1ULL);

    idx = hype_paging_map_mmio_1gb_into_pdpt(g_pdpt, g_mmio_pd, bar);

    CHECK_HEX("pdpt index = 384 % 512", 384u, idx);
    CHECK_HEX("pdpt[384] present+write", HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE,
              g_pdpt[384] & 0xFFFULL);
    CHECK_HEX("pdpt[384] -> the supplied pd", (uint64_t)g_mmio_pd,
              g_pdpt[384] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("pd[0] present+write+PS+PCD (device registers must be uncacheable)",
              HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE | HYPE_PAGING_PS | HYPE_PAGING_PCD,
              g_mmio_pd[0] & 0xFFFULL);
    CHECK_HEX("pd[0] maps the containing GB, not the BAR offset", gb_base,
              g_mmio_pd[0] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("pd[511] covers the top of that GB", gb_base + 511ULL * HYPE_PAGING_2MB,
              g_mmio_pd[511] & 0x000FFFFFFFFFF000ULL);

    /* The reason this helper exists: nothing else in the live tables moved. */
    CHECK_HEX("pml4[0] untouched (map_mmio_1gb would have replaced it)",
              (uint64_t)g_pdpt, g_pml4[0] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("pdpt[0] (low identity GB 0) still present", HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE,
              g_pdpt[0] & 0xFFFULL);
    CHECK_HEX("pdpt[0] still points at the identity pd", (uint64_t)g_pd,
              g_pdpt[0] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("pdpt[3] (last mapped identity GB) still present",
              HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE, g_pdpt[3] & 0xFFFULL);
    CHECK_HEX("an unrelated unmapped slot stays absent", 0u, g_pdpt[100] & 1ULL);
}

/*
 * PERF-2 for the high-mapped framebuffer (the Intel i5-13420H case). The point
 * is the relative indexing: pd_tables slot 0 is the GB containing base, so a
 * 256GB framebuffer must land in g_fb_pd[0], not g_fb_pd[256] (which would be
 * out of bounds -- the bug this replaces).
 */
static void test_mark_region_wc_relative_high_framebuffer(void) {
    uint64_t fb_base = 0x4000000000ULL; /* 256 GiB, as reported by that firmware */
    uint64_t fb_size = 0x7e9000ULL;     /* ~8.3 MB -> 4 x 2MB pages */
    unsigned int mapped;

    hype_paging_build_identity(g_pml4, g_pdpt, g_pd, 4);
    mapped = hype_paging_map_region_2mb(g_pdpt, g_fb_pd, fb_base, fb_size);
    CHECK_HEX("framebuffer maps into one GB slot", 1u, mapped);
    CHECK_HEX("precondition: mapped as plain WB (no PWT)", 0u, g_fb_pd[0][0] & HYPE_PAGING_PWT);

    hype_paging_mark_region_wc_relative(g_fb_pd, fb_base, fb_size, mapped);

    /* 0x7e9000 spans 2MB pages 0..3 within the GB. */
    CHECK_HEX("pd[0] now PWT (PAT slot 1 = WC)", HYPE_PAGING_PWT, g_fb_pd[0][0] & HYPE_PAGING_PWT);
    CHECK_HEX("pd[0] PCD cleared (PWT alone selects WC, not UC)", 0u, g_fb_pd[0][0] & HYPE_PAGING_PCD);
    CHECK_HEX("pd[3] (last page of the ~8.3MB region) PWT", HYPE_PAGING_PWT,
              g_fb_pd[0][3] & HYPE_PAGING_PWT);
    CHECK_HEX("pd[0] still present+write+PS", HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE | HYPE_PAGING_PS,
              g_fb_pd[0][0] & (HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE | HYPE_PAGING_PS));
    CHECK_HEX("pd[0] address unchanged", fb_base, g_fb_pd[0][0] & 0x000FFFFFFFFFF000ULL);
    /* Pages beyond the framebuffer keep the default type. */
    CHECK_HEX("pd[4] (past the region) untouched", 0u, g_fb_pd[0][4] & HYPE_PAGING_PWT);
    /* Zero size is a no-op, not a stray write. */
    hype_paging_mark_region_wc_relative(g_fb_pd, fb_base, 0, mapped);
    CHECK_HEX("zero size leaves pd[4] alone", 0u, g_fb_pd[0][4] & HYPE_PAGING_PWT);
}

static void test_mark_region_wc(void) {
    /* PERF-2 (#234): OR PWT into the 2MB PDEs covering the framebuffer, clear PCD,
     * leave everything else (and other pages) untouched. */
    static hype_pte_t pml4[HYPE_PAGING_ENTRIES_PER_TABLE];
    static hype_pte_t pdpt[HYPE_PAGING_ENTRIES_PER_TABLE];
    static hype_pte_t pd[8][HYPE_PAGING_ENTRIES_PER_TABLE];
    uint64_t fb = 0xE0000000ULL, sz = 0x7E9000ULL; /* real AMD-laptop FB: spans 4 2MB pages */
    unsigned gb = 3, first = 256, last = 259, i;

    hype_paging_build_identity(pml4, pdpt, pd, 8);
    /* Pre-set PCD on the first FB page to prove it's cleared when WC is applied. */
    pd[gb][first] |= HYPE_PAGING_PCD;
    hype_paging_mark_region_wc(pd, fb, sz, 8);

    for (i = first; i <= last; i++) {
        CHECK_HEX("wc: PWT set on covering page", HYPE_PAGING_PWT, pd[gb][i] & HYPE_PAGING_PWT);
        CHECK_HEX("wc: PCD cleared on covering page", 0, pd[gb][i] & HYPE_PAGING_PCD);
        CHECK_HEX("wc: page still present+PS", HYPE_PAGING_PRESENT | HYPE_PAGING_PS,
                  pd[gb][i] & (HYPE_PAGING_PRESENT | HYPE_PAGING_PS));
    }
    /* Neighbours untouched. */
    CHECK_HEX("wc: page before region has no PWT", 0, pd[gb][first - 1] & HYPE_PAGING_PWT);
    CHECK_HEX("wc: page after region has no PWT", 0, pd[gb][last + 1] & HYPE_PAGING_PWT);

    /* size==0 is a no-op; a region beyond gb_mapped stops at the bound (no OOB). */
    hype_paging_mark_region_wc(pd, fb, 0, 8);
    hype_paging_mark_region_wc(pd, (uint64_t)8 * HYPE_PAGING_1GB, HYPE_PAGING_2MB, 8);
    CHECK_HEX("wc: zero-size left page unchanged", HYPE_PAGING_PWT,
              pd[gb][first] & HYPE_PAGING_PWT);
}

static void test_apply_nx_exempts_image_range(void) {
    hype_pte_t pd[4][HYPE_PAGING_ENTRIES_PER_TABLE];
    /* Image spans the tail of GB0 into the head of GB1 -- exercises the
     * cross-GB overlap arithmetic, not just a single-GB case. */
    uint64_t image_base = HYPE_PAGING_1GB - HYPE_PAGING_2MB;
    uint64_t image_size = 3 * HYPE_PAGING_2MB; /* covers GB0's last page + GB1's first two */
    unsigned int gb, j;

    hype_paging_build_identity(g_pml4, g_pdpt, pd, 4);
    { hype_exec_range_t ex[1] = {{image_base, image_size}};
      hype_paging_apply_nx(pd, 4, ex, 1); }

    for (gb = 0; gb < 4; gb++) {
        for (j = 0; j < HYPE_PAGING_ENTRIES_PER_TABLE; j++) {
            int in_image = (gb == 0 && j == 511) || (gb == 1 && (j == 0 || j == 1));
            uint64_t nx = pd[gb][j] & HYPE_PAGING_NX;
            if (in_image) {
                CHECK_HEX("apply_nx: image page stays executable", 0, nx);
            } else {
                CHECK_HEX("apply_nx: non-image page gets NX", HYPE_PAGING_NX, nx);
            }
            /* NX must never disturb PRESENT/WRITE/PS -- otherwise a caller that
             * only wanted "not executable" would also silently unmap or read-only
             * pages it never asked to touch. */
            CHECK_HEX("apply_nx: PRESENT/WRITE/PS untouched",
                      HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE | HYPE_PAGING_PS,
                      pd[gb][j] & (HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE | HYPE_PAGING_PS));
        }
    }
}

static void test_apply_nx_zero_size_exempts_nothing(void) {
    /* exec_size == 0 -- e.g. a failed image-base query the caller should have
     * already treated as fatal, but the function itself must not silently
     * exempt some arbitrary page if it is ever reached with this input. */
    hype_pte_t pd[2][HYPE_PAGING_ENTRIES_PER_TABLE];
    unsigned int j;

    hype_paging_build_identity(g_pml4, g_pdpt, pd, 2);
    hype_paging_apply_nx(pd, 2, 0, 0);

    for (j = 0; j < HYPE_PAGING_ENTRIES_PER_TABLE; j++) {
        CHECK_HEX("apply_nx: zero exec_size marks every page NX", HYPE_PAGING_NX,
                  pd[0][j] & HYPE_PAGING_NX);
        CHECK_HEX("apply_nx: zero exec_size marks every page NX (gb1)", HYPE_PAGING_NX,
                  pd[1][j] & HYPE_PAGING_NX);
    }
}

static void test_apply_nx_stops_at_gb_mapped(void) {
    /* Only pd_tables[0..gb_mapped-1] is caller-owned; entries beyond that
     * bound must never be touched (same discipline as hype_paging_mark_region_wc's
     * gb_mapped bound). */
    hype_pte_t pd[2][HYPE_PAGING_ENTRIES_PER_TABLE];

    hype_paging_build_identity(g_pml4, g_pdpt, pd, 2);
    hype_paging_apply_nx(pd, 1, 0, 0);

    CHECK_HEX("apply_nx: gb0 (within bound) gets NX", HYPE_PAGING_NX, pd[0][0] & HYPE_PAGING_NX);
    CHECK_HEX("apply_nx: gb1 (beyond gb_mapped) left alone", 0, pd[1][0] & HYPE_PAGING_NX);
}

static void test_apply_nx_exempts_second_range(void) {
    /* The AP trampoline page: a second, independent exempt range distinct from the image. */
    hype_pte_t pd[2][HYPE_PAGING_ENTRIES_PER_TABLE];
    uint64_t tramp_page = 5 * HYPE_PAGING_2MB; /* gb0, index 5 */

    hype_paging_build_identity(g_pml4, g_pdpt, pd, 2);
    { hype_exec_range_t ex[2] = {{HYPE_PAGING_1GB, HYPE_PAGING_2MB}, {tramp_page, 4096ULL}};
      hype_paging_apply_nx(pd, 2, ex, 2); }

    CHECK_HEX("apply_nx: second-range page stays executable", 0, pd[0][5] & HYPE_PAGING_NX);
    CHECK_HEX("apply_nx: first-range page stays executable", 0, pd[1][0] & HYPE_PAGING_NX);
    CHECK_HEX("apply_nx: page in neither range gets NX", HYPE_PAGING_NX, pd[0][4] & HYPE_PAGING_NX);
}

/*
 * Boot 36 regression (2026-08-30).
 *
 * The NX pass took exactly two exempt ranges, so UEFI's RuntimeServicesCode was marked
 * no-execute -- and hype calls ResetSystem() through it to reboot the host. The panic was a
 * page fault with rip == cr2 = 0xddbbb668 and error_code=0x11 (present page, instruction
 * fetch), inside `RuntimeServicesCode phys=0xddb49000 pages=182`.
 *
 * The shape that matters: several disjoint ranges, all kept executable, everything else NX.
 */
static void test_nx_exempts_several_ranges(void) {
    static hype_pte_t pd[4][HYPE_PAGING_ENTRIES_PER_TABLE];
    /* hype's image, the AP trampoline, and a firmware runtime-code region -- the boot 36 set. */
    hype_exec_range_t ex[3] = {
        {0x140000000ULL, 0x1ce1000ULL}, /* image */
        {0x8000ULL, 4096ULL},           /* trampoline */
        {0xddb49000ULL, 182ULL * 4096ULL},
    };
    unsigned int gb, j;

    for (gb = 0; gb < 4; gb++) {
        for (j = 0; j < HYPE_PAGING_ENTRIES_PER_TABLE; j++) {
            pd[gb][j] = HYPE_PAGING_PRESENT;
        }
    }
    hype_paging_apply_nx(pd, 4, ex, 3);

    /* The faulting address itself must be executable. */
    CHECK_HEX("runtime-services code stays executable", 0,
              pd[3][(0xddbbb668ULL % HYPE_PAGING_1GB) / HYPE_PAGING_2MB] & HYPE_PAGING_NX);
    /* Both ends of that region, since it spans more than one 2 MiB leaf. */
    CHECK_HEX("runtime region first leaf executable", 0,
              pd[3][(0xddb49000ULL % HYPE_PAGING_1GB) / HYPE_PAGING_2MB] & HYPE_PAGING_NX);
    CHECK_HEX("runtime region last leaf executable", 0,
              pd[3][((0xddb49000ULL + 182ULL * 4096ULL - 1ULL) % HYPE_PAGING_1GB)
                    / HYPE_PAGING_2MB] & HYPE_PAGING_NX);
    /* The trampoline and a page inside the image, both still exempt. */
    CHECK_HEX("trampoline stays executable", 0, pd[0][0] & HYPE_PAGING_NX);
    /* And an ordinary page well away from every range is NX. */
    CHECK_HEX("an unexempt page is NX", HYPE_PAGING_NX, pd[1][100] & HYPE_PAGING_NX);
    CHECK_HEX("a page just below the runtime region is NX", HYPE_PAGING_NX,
              pd[3][((0xddb49000ULL % HYPE_PAGING_1GB) / HYPE_PAGING_2MB) - 1u]
                  & HYPE_PAGING_NX);

    /* A zero-size slot must match nothing rather than exempting the world. */
    for (gb = 0; gb < 4; gb++) {
        for (j = 0; j < HYPE_PAGING_ENTRIES_PER_TABLE; j++) pd[gb][j] = HYPE_PAGING_PRESENT;
    }
    ex[0].size = 0;
    hype_paging_apply_nx(pd, 1, ex, 3);
    CHECK_HEX("a zero-size exempt slot exempts nothing", HYPE_PAGING_NX,
              pd[0][100] & HYPE_PAGING_NX);
}

int main(void) {
    test_encode_entry();
    test_mark_region_wc();
    test_map_mmio_1gb_high();
    test_map_mmio_1gb_into_pdpt_preserves_low_map();
    test_mark_region_wc_relative_high_framebuffer();
    test_encode_entry_masks_low_bits_of_address();
    test_encode_entry_nx_bit();
    test_encode_entry_flags_masked_to_allowed_bits();
    test_build_identity();
    test_map_region_high_framebuffer();
    test_map_region_straddling_gb_boundary();
    test_map_region_out_of_range_and_empty();
    test_apply_nx_exempts_image_range();
    test_apply_nx_zero_size_exempts_nothing();
    test_apply_nx_stops_at_gb_mapped();
    test_apply_nx_exempts_second_range();

    test_nx_exempts_several_ranges();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
