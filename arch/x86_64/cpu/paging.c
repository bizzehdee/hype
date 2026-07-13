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
