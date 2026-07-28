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

/*
 * VMX-4 (#236): the remap / not-present operations a live guest needs. These
 * are why the FW-1 guest can exist on Intel at all -- its RAM is not
 * identity-mapped, so an identity EPT cannot describe it.
 *
 * The load-bearing property is that pd_tables is indexed by the GUEST-physical
 * address while the entry contents come from the HOST address. Getting that
 * backwards still produces a plausible-looking table, so it is asserted
 * directly with two addresses that cannot be confused.
 */
static void test_map_range_remaps_guest_to_host(void) {
    unsigned int i;
    uint64_t guest_base = 0;              /* guest sees its RAM at 0 ... */
    uint64_t host_base = 0x37E00000ULL;   /* ... but it is really up here */
    uint64_t size = 4ULL * HYPE_PAGING_2MB;

    hype_ept_build_identity(g_pml4, g_pdpt, g_pd, 4);
    hype_ept_map_range(g_pd, guest_base, host_base, size);

    CHECK_HEX("map_range: guest 0 -> host base", host_base,
              g_pd[0][0] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("map_range: guest +2MB -> host +2MB", host_base + HYPE_PAGING_2MB,
              g_pd[0][1] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("map_range: guest +6MB -> host +6MB", host_base + 3ULL * HYPE_PAGING_2MB,
              g_pd[0][3] & 0x000FFFFFFFFFF000ULL);
    /* R/W/X + WB + PS, exactly as build_identity grants -- a remapped page must
     * not silently lose permissions or become UC. */
    CHECK_HEX("map_range: entry keeps R/W/X + WB + PS",
              HYPE_EPT_READ | HYPE_EPT_WRITE | HYPE_EPT_EXEC | HYPE_EPT_MEMTYPE_WB | HYPE_EPT_PS,
              g_pd[0][0] & 0xFFULL);
    /* One past the mapped range must still be whatever identity left there. */
    CHECK_HEX("map_range: first page past the range is untouched",
              (uint64_t)4 * HYPE_PAGING_2MB, g_pd[0][4] & 0x000FFFFFFFFFF000ULL);

    for (i = 0; i < 4u; i++) {
        if ((g_pd[0][i] & 0x000FFFFFFFFFF000ULL) != host_base + (uint64_t)i * HYPE_PAGING_2MB) {
            printf("FAIL: map_range pd[0][%u] not remapped\n", i);
            failures++;
            break;
        }
    }
}

/* The flash window: a range high in guest-physical space mapped somewhere
 * unrelated in host space, which is how FW-1 presents OVMF just under 4GB. */
static void test_map_range_high_guest_address(void) {
    uint64_t size = 2ULL * HYPE_PAGING_2MB;
    uint64_t guest_base = 0x100000000ULL - size; /* just under 4GB */
    uint64_t host_base = 0x7DE00000ULL;

    hype_ept_build_identity(g_pml4, g_pdpt, g_pd, 4);
    hype_ept_map_range(g_pd, guest_base, host_base, size);

    /* guest_base is in the 4th GB (index 3), at the top of its PD. */
    CHECK_HEX("map_range: high guest address lands in pd[3]", host_base,
              g_pd[3][510] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("map_range: high guest address, second page", host_base + HYPE_PAGING_2MB,
              g_pd[3][511] & 0x000FFFFFFFFFF000ULL);
}

/*
 * Not-present holes. Zero is the not-present encoding: with R/W/X all clear the
 * entry grants nothing, so any guest access raises an EPT violation and reaches
 * hype's device models -- which is the entire point of punching MMIO holes.
 */
static void test_mark_not_present(void) {
    hype_ept_build_identity(g_pml4, g_pdpt, g_pd, 4);

    hype_ept_mark_not_present(g_pd, 2ULL * HYPE_PAGING_2MB);
    CHECK_HEX("mark_not_present: entry is zero (grants no R/W/X)", 0, g_pd[0][2]);
    CHECK_HEX("mark_not_present: neighbour below untouched", HYPE_PAGING_2MB,
              g_pd[0][1] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("mark_not_present: neighbour above untouched", 3ULL * HYPE_PAGING_2MB,
              g_pd[0][3] & 0x000FFFFFFFFFF000ULL);

    /* Across a GB boundary, to pin the pd_tables[gb] index arithmetic. */
    hype_ept_mark_not_present(g_pd, HYPE_PAGING_1GB + 5ULL * HYPE_PAGING_2MB);
    CHECK_HEX("mark_not_present: indexes the right GB table", 0, g_pd[1][5]);
}

static void test_mark_range_not_present(void) {
    unsigned int i;
    uint64_t base = 8ULL * HYPE_PAGING_2MB;
    uint64_t size = 3ULL * HYPE_PAGING_2MB;

    hype_ept_build_identity(g_pml4, g_pdpt, g_pd, 4);
    hype_ept_mark_range_not_present(g_pd, base, size);

    for (i = 8; i < 11u; i++) {
        if (g_pd[0][i] != 0) {
            printf("FAIL: mark_range_not_present left pd[0][%u] present\n", i);
            failures++;
            break;
        }
    }
    CHECK_HEX("mark_range: page before the range still present", 7ULL * HYPE_PAGING_2MB,
              g_pd[0][7] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("mark_range: page after the range still present", 11ULL * HYPE_PAGING_2MB,
              g_pd[0][11] & 0x000FFFFFFFFFF000ULL);
}

/*
 * The exact FW-1 layout, in the order boot/main.c applies it: identity, then
 * RAM at guest 0, then the flash window just under 4GB, then not-present
 * everything between. Asserted end-to-end because the ORDER matters -- the
 * not-present sweep runs last and must not clobber either mapped range.
 */
static void test_fw1_layout_end_to_end(void) {
    uint64_t ram_size = 4ULL * HYPE_PAGING_2MB;
    uint64_t ram_host = 0x37E00000ULL;
    uint64_t flash_size = 2ULL * HYPE_PAGING_2MB;
    uint64_t flash_guest = 0x100000000ULL - flash_size;
    uint64_t flash_host = 0x7DE00000ULL;

    hype_ept_build_identity(g_pml4, g_pdpt, g_pd, 4);
    hype_ept_map_range(g_pd, 0, ram_host, ram_size);
    hype_ept_map_range(g_pd, flash_guest, flash_host, flash_size);
    hype_ept_mark_range_not_present(g_pd, ram_size, flash_guest - ram_size);

    CHECK_HEX("fw1 layout: guest RAM still mapped after the hole sweep", ram_host,
              g_pd[0][0] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("fw1 layout: last RAM page still mapped", ram_host + 3ULL * HYPE_PAGING_2MB,
              g_pd[0][3] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("fw1 layout: flash window still mapped", flash_host,
              g_pd[3][510] & 0x000FFFFFFFFFF000ULL);
    CHECK_HEX("fw1 layout: gap immediately above RAM is a hole", 0, g_pd[0][4]);
    CHECK_HEX("fw1 layout: gap in the middle is a hole", 0, g_pd[1][0]);
    CHECK_HEX("fw1 layout: gap just below the flash window is a hole", 0, g_pd[3][509]);
}

int main(void) {
    test_encode_entry();
    test_encode_entry_masks_low_bits_of_address();
    test_encode_entry_flags_masked_to_allowed_bits();
    test_build_identity();
    test_map_range_remaps_guest_to_host();
    test_map_range_high_guest_address();
    test_mark_not_present();
    test_mark_range_not_present();
    test_fw1_layout_end_to_end();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
