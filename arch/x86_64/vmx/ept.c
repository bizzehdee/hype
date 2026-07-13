#include "ept.h"

#include "../cpu/paging.h"

uint64_t hype_ept_encode_entry(uint64_t addr, uint64_t flags) {
    return (addr & 0x000FFFFFFFFFF000ULL) | (flags & 0xFFULL);
}

void hype_ept_build_identity(hype_ept_pte_t *pml4, hype_ept_pte_t *pdpt,
                              hype_ept_pte_t pd_tables[][HYPE_EPT_ENTRIES_PER_TABLE],
                              unsigned int gb_to_map) {
    unsigned int i, j;
    uint64_t table_flags = HYPE_EPT_READ | HYPE_EPT_WRITE | HYPE_EPT_EXEC;
    uint64_t page_flags = table_flags | HYPE_EPT_MEMTYPE_WB | HYPE_EPT_PS;

    for (i = 0; i < HYPE_EPT_ENTRIES_PER_TABLE; i++) {
        pml4[i] = 0;
        pdpt[i] = 0;
    }

    pml4[0] = hype_ept_encode_entry((uint64_t)pdpt, table_flags);

    for (i = 0; i < gb_to_map; i++) {
        pdpt[i] = hype_ept_encode_entry((uint64_t)pd_tables[i], table_flags);
        for (j = 0; j < HYPE_EPT_ENTRIES_PER_TABLE; j++) {
            uint64_t phys = (uint64_t)i * HYPE_PAGING_1GB + (uint64_t)j * HYPE_PAGING_2MB;
            pd_tables[i][j] = hype_ept_encode_entry(phys, page_flags);
        }
    }
}
