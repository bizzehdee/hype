#include "npt.h"

void hype_npt_build_identity(hype_pte_t *pml4, hype_pte_t *pdpt,
                              hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE],
                              unsigned int gb_to_map) {
    unsigned int i, j;
    uint64_t table_flags = HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE | HYPE_PAGING_USER;
    uint64_t page_flags = table_flags | HYPE_PAGING_PS;

    for (i = 0; i < HYPE_PAGING_ENTRIES_PER_TABLE; i++) {
        pml4[i] = 0;
        pdpt[i] = 0;
    }

    pml4[0] = hype_paging_encode_entry((uint64_t)pdpt, table_flags);

    for (i = 0; i < gb_to_map; i++) {
        pdpt[i] = hype_paging_encode_entry((uint64_t)pd_tables[i], table_flags);
        for (j = 0; j < HYPE_PAGING_ENTRIES_PER_TABLE; j++) {
            uint64_t phys = (uint64_t)i * HYPE_PAGING_1GB + (uint64_t)j * HYPE_PAGING_2MB;
            pd_tables[i][j] = hype_paging_encode_entry(phys, page_flags);
        }
    }
}

void hype_npt_mark_not_present(hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE], uint64_t phys_addr) {
    unsigned int gb = (unsigned int)(phys_addr / HYPE_PAGING_1GB);
    unsigned int pd_index = (unsigned int)((phys_addr % HYPE_PAGING_1GB) / HYPE_PAGING_2MB);

    pd_tables[gb][pd_index] = 0;
}

void hype_npt_mark_range_not_present(hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE], uint64_t base,
                                      uint64_t size) {
    uint64_t offset;

    for (offset = 0; offset < size; offset += HYPE_PAGING_2MB) {
        hype_npt_mark_not_present(pd_tables, base + offset);
    }
}

void hype_npt_map_range(hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE], uint64_t guest_phys_base,
                         uint64_t host_phys_base, uint64_t size) {
    uint64_t table_flags = HYPE_PAGING_PRESENT | HYPE_PAGING_WRITE | HYPE_PAGING_USER;
    uint64_t page_flags = table_flags | HYPE_PAGING_PS;
    uint64_t offset;

    for (offset = 0; offset < size; offset += HYPE_PAGING_2MB) {
        uint64_t guest_phys = guest_phys_base + offset;
        uint64_t host_phys = host_phys_base + offset;
        unsigned int gb = (unsigned int)(guest_phys / HYPE_PAGING_1GB);
        unsigned int pd_index = (unsigned int)((guest_phys % HYPE_PAGING_1GB) / HYPE_PAGING_2MB);

        pd_tables[gb][pd_index] = hype_paging_encode_entry(host_phys, page_flags);
    }
}

void hype_npt_map_range_ro(hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE],
                           uint64_t guest_phys_base, uint64_t host_phys_base, uint64_t size) {
    /* #457: leaf entries WITHOUT the writable bit. Reads and instruction fetches translate
     * directly at full speed; a guest write takes #VMEXIT_NPF with EXITINFO1's write bit set.
     * The upper table levels keep W from the identity build -- NPT permissions AND together
     * down the walk, so clearing it at the leaf alone is what forbids the write. */
    uint64_t page_flags = HYPE_PAGING_PRESENT | HYPE_PAGING_USER | HYPE_PAGING_PS;
    uint64_t offset;

    for (offset = 0; offset < size; offset += HYPE_PAGING_2MB) {
        uint64_t guest_phys = guest_phys_base + offset;
        uint64_t host_phys = host_phys_base + offset;
        unsigned int gb = (unsigned int)(guest_phys / HYPE_PAGING_1GB);
        unsigned int pd_index = (unsigned int)((guest_phys % HYPE_PAGING_1GB) / HYPE_PAGING_2MB);

        pd_tables[gb][pd_index] = hype_paging_encode_entry(host_phys, page_flags);
    }
}
