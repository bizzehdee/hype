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

void hype_paging_build_identity_at(void *gpa0_host, uint64_t pml4_gpa, uint64_t pdpt_gpa,
                                   uint64_t pd0_gpa, unsigned int gb_to_map) {
    unsigned char *base = (unsigned char *)gpa0_host;
    hype_pte_t *pml4 = (hype_pte_t *)(base + pml4_gpa);
    hype_pte_t *pdpt = (hype_pte_t *)(base + pdpt_gpa);
    unsigned int i, j;

    for (i = 0; i < HYPE_PAGING_ENTRIES_PER_TABLE; i++) {
        pml4[i] = 0;
        pdpt[i] = 0;
    }

    pml4[0] = hype_paging_encode_entry(pdpt_gpa, HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE);

    for (i = 0; i < gb_to_map; i++) {
        uint64_t pd_gpa = pd0_gpa + (uint64_t)i * 4096ull;
        hype_pte_t *pd = (hype_pte_t *)(base + pd_gpa);

        pdpt[i] = hype_paging_encode_entry(pd_gpa, HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE);
        for (j = 0; j < HYPE_PAGING_ENTRIES_PER_TABLE; j++) {
            uint64_t phys = (uint64_t)i * HYPE_PAGING_1GB + (uint64_t)j * HYPE_PAGING_2MB;
            pd[j] = hype_paging_encode_entry(phys,
                                             HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE | HYPE_PAGING_PS);
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

void hype_paging_mark_region_wc_relative(hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE],
                                          uint64_t base, uint64_t size, unsigned int gb_mapped) {
    uint64_t first, last, p, base_gb;

    if (size == 0) {
        return;
    }
    /* Slot 0 of pd_tables is the GB containing `base` -- the only difference
     * from hype_paging_mark_region_wc, and the whole reason this exists. */
    base_gb = base / HYPE_PAGING_1GB;
    first = base & ~(HYPE_PAGING_2MB - 1ULL);
    last = (base + size - 1ULL) & ~(HYPE_PAGING_2MB - 1ULL);
    for (p = first; p <= last; p += HYPE_PAGING_2MB) {
        unsigned int gb_rel = (unsigned int)((p / HYPE_PAGING_1GB) - base_gb);
        unsigned int idx = (unsigned int)((p % HYPE_PAGING_1GB) / HYPE_PAGING_2MB);
        if (gb_rel >= gb_mapped) {
            break;
        }
        /* Only a present 2MB page gets WC; leave holes untouched. */
        if (pd_tables[gb_rel][idx] & HYPE_PAGING_PRESENT) {
            pd_tables[gb_rel][idx] = (pd_tables[gb_rel][idx] & ~HYPE_PAGING_PCD) | HYPE_PAGING_PWT;
        }
    }
}

unsigned int hype_paging_map_mmio_1gb_into_pdpt(hype_pte_t *pdpt, hype_pte_t *pd, uint64_t phys) {
    uint64_t gb = phys / HYPE_PAGING_1GB;
    unsigned int pdpt_idx = (unsigned int)(gb % HYPE_PAGING_ENTRIES_PER_TABLE);
    uint64_t base = gb * HYPE_PAGING_1GB;
    unsigned int j;

    /* Deliberately NOT zeroing pdpt: every other GB slot in it is the live low
     * identity map. Only this one entry changes. */
    for (j = 0; j < HYPE_PAGING_ENTRIES_PER_TABLE; j++) {
        pd[j] = hype_paging_encode_entry(base + (uint64_t)j * HYPE_PAGING_2MB,
                                         HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE |
                                             HYPE_PAGING_PS | HYPE_PAGING_PCD);
    }
    pdpt[pdpt_idx] = hype_paging_encode_entry((uint64_t)pd, HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE);
    return pdpt_idx;
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

void hype_paging_apply_nx(hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE],
                          unsigned int gb_mapped, const hype_exec_range_t *exempt,
                          unsigned int n_exempt) {
    unsigned int gb, j, k;

    if (exempt == 0) {
        n_exempt = 0;
    }
    if (n_exempt > HYPE_PAGING_MAX_EXEC_RANGES) {
        n_exempt = HYPE_PAGING_MAX_EXEC_RANGES;
    }
    for (gb = 0; gb < gb_mapped; gb++) {
        for (j = 0; j < HYPE_PAGING_ENTRIES_PER_TABLE; j++) {
            uint64_t phys = (uint64_t)gb * HYPE_PAGING_1GB + (uint64_t)j * HYPE_PAGING_2MB;
            int keep_exec = 0;

            if ((pd_tables[gb][j] & HYPE_PAGING_PRESENT) == 0) {
                continue;
            }
            /*
             * A range is compared at 2 MiB granularity: any leaf that OVERLAPS an exempt
             * range stays executable, because that is the mapping granularity available. A
             * range of size 0 is an unused slot and matches nothing.
             */
            for (k = 0; k < n_exempt; k++) {
                uint64_t first, last;
                if (exempt[k].size == 0) {
                    continue;
                }
                first = exempt[k].base & ~(HYPE_PAGING_2MB - 1ULL);
                last = (exempt[k].base + exempt[k].size - 1ULL) & ~(HYPE_PAGING_2MB - 1ULL);
                if (phys >= first && phys <= last) {
                    keep_exec = 1;
                    break;
                }
            }
            if (keep_exec) {
                continue;
            }
            pd_tables[gb][j] |= HYPE_PAGING_NX;
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
