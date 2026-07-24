#include "paging.h"

uint64_t hype_paging_encode_entry(uint64_t addr, uint64_t flags) {
    return (addr & 0x000FFFFFFFFFF000ULL) | (flags & 0x8000000000000FFFULL);
}

void hype_paging_build_identity(hype_pte_t *pml4, hype_pte_t *pdpt,
                                 hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE],
                                 unsigned int gb_to_map) {
    unsigned int i, j;

    for (i = 0; i < HYPE_PAGING_ENTRIES_PER_TABLE; i++) {
        pml4[i] = 0;
        pdpt[i] = 0;
    }

    pml4[0] = hype_paging_encode_entry((uint64_t)pdpt, HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE);

    for (i = 0; i < gb_to_map; i++) {
        pdpt[i] = hype_paging_encode_entry((uint64_t)pd_tables[i], HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE);
        for (j = 0; j < HYPE_PAGING_ENTRIES_PER_TABLE; j++) {
            uint64_t phys = (uint64_t)i * HYPE_PAGING_1GB + (uint64_t)j * HYPE_PAGING_2MB;
            pd_tables[i][j] = hype_paging_encode_entry(phys, HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE | HYPE_PAGING_PS);
        }
    }
}

unsigned int hype_paging_map_mmio_1gb(hype_pte_t *pml4, hype_pte_t *pdpt, hype_pte_t *pd,
                                       uint64_t phys) {
    uint64_t gb = phys / HYPE_PAGING_1GB;
    unsigned int pml4_idx = (unsigned int)(gb / HYPE_PAGING_ENTRIES_PER_TABLE);
    unsigned int pdpt_idx = (unsigned int)(gb % HYPE_PAGING_ENTRIES_PER_TABLE);
    uint64_t base = gb * HYPE_PAGING_1GB;
    unsigned int j;

    for (j = 0; j < HYPE_PAGING_ENTRIES_PER_TABLE; j++) {
        pdpt[j] = 0; /* only pdpt_idx becomes present */
    }
    for (j = 0; j < HYPE_PAGING_ENTRIES_PER_TABLE; j++) {
        pd[j] = hype_paging_encode_entry(base + (uint64_t)j * HYPE_PAGING_2MB,
                                         HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE |
                                             HYPE_PAGING_PS | HYPE_PAGING_PCD);
    }
    pdpt[pdpt_idx] = hype_paging_encode_entry((uint64_t)pd, HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE);
    pml4[pml4_idx] = hype_paging_encode_entry((uint64_t)pdpt, HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE);
    return pml4_idx;
}

void hype_paging_mark_region_wc(hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE],
                                uint64_t base, uint64_t size, unsigned int gb_mapped) {
    uint64_t first, last, p;

    if (size == 0) {
        return;
    }
    first = base & ~(HYPE_PAGING_2MB - 1ULL);
    last = (base + size - 1ULL) & ~(HYPE_PAGING_2MB - 1ULL);
    for (p = first; p <= last; p += HYPE_PAGING_2MB) {
        unsigned int gb = (unsigned int)(p / HYPE_PAGING_1GB);
        unsigned int idx = (unsigned int)((p % HYPE_PAGING_1GB) / HYPE_PAGING_2MB);
        if (gb >= gb_mapped) {
            break;
        }
        /* Only a present 2MB page gets WC; leave holes untouched. */
        if (pd_tables[gb][idx] & HYPE_PAGING_PRESENT) {
            pd_tables[gb][idx] = (pd_tables[gb][idx] & ~HYPE_PAGING_PCD) | HYPE_PAGING_PWT;
        }
    }
}

unsigned int hype_paging_map_region_2mb(hype_pte_t *pdpt,
                                         hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE],
                                         uint64_t phys_base, uint64_t size) {
    uint64_t first_gb, last_gb, gb;
    unsigned int n;

    if (size == 0) {
        return 0;
    }
    first_gb = phys_base / HYPE_PAGING_1GB;
    last_gb = (phys_base + size - 1) / HYPE_PAGING_1GB;
    /* PML4[0] (built by hype_paging_build_identity) spans [0, 512GB); a
     * region needing a higher PML4 entry is out of scope for this
     * single-table helper. */
    if (last_gb >= HYPE_PAGING_ENTRIES_PER_TABLE) {
        return 0;
    }

    n = 0;
    for (gb = first_gb; gb <= last_gb; gb++, n++) {
        hype_pte_t *pd = pd_tables[n];
        unsigned int j;
        pdpt[gb] = hype_paging_encode_entry((uint64_t)pd, HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE);
        for (j = 0; j < HYPE_PAGING_ENTRIES_PER_TABLE; j++) {
            uint64_t phys = gb * HYPE_PAGING_1GB + (uint64_t)j * HYPE_PAGING_2MB;
            pd[j] = hype_paging_encode_entry(phys, HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE | HYPE_PAGING_PS);
        }
    }
    return n;
}
