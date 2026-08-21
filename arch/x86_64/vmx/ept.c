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

/*
 * VMX-4 (#236): remap a guest-physical range onto different host-physical
 * memory, and punch not-present holes. The EPT counterparts of
 * hype_npt_map_range() / hype_npt_mark_range_not_present(), and needed for the
 * same reason: a live guest's RAM is NOT identity-mapped (FW-1 allocates host
 * RAM wherever UEFI gives it and presents it to the guest at guest-physical 0),
 * and MMIO windows must fault so hype's device models see them.
 *
 * Identical index arithmetic to the NPT versions -- 2MB pages, pd_tables[gb]
 * selected by the GUEST-physical address, entry contents built from the HOST
 * address. Only the entry encoding differs (R/W/X + memory type rather than
 * Present/Write/User), which hype_ept_encode_entry() already handles.
 *
 * Callers must keep `size` a whole multiple of 2MB and both bases 2MB-aligned,
 * exactly as the NPT versions require.
 */
void hype_ept_map_range(hype_ept_pte_t pd_tables[][HYPE_EPT_ENTRIES_PER_TABLE],
                        uint64_t guest_phys_base, uint64_t host_phys_base, uint64_t size) {
    uint64_t page_flags =
        HYPE_EPT_READ | HYPE_EPT_WRITE | HYPE_EPT_EXEC | HYPE_EPT_MEMTYPE_WB | HYPE_EPT_PS;
    uint64_t offset;

    for (offset = 0; offset < size; offset += HYPE_PAGING_2MB) {
        uint64_t guest_phys = guest_phys_base + offset;
        uint64_t host_phys = host_phys_base + offset;
        unsigned int gb = (unsigned int)(guest_phys / HYPE_PAGING_1GB);
        unsigned int pd_index = (unsigned int)((guest_phys % HYPE_PAGING_1GB) / HYPE_PAGING_2MB);

        pd_tables[gb][pd_index] = hype_ept_encode_entry(host_phys, page_flags);
    }
}

void hype_ept_map_range_ro(hype_ept_pte_t pd_tables[][HYPE_EPT_ENTRIES_PER_TABLE],
                           uint64_t guest_phys_base, uint64_t host_phys_base, uint64_t size) {
    /* #457: the EPT twin of hype_npt_map_range_ro -- read+execute, no write, so a guest write
     * raises an EPT violation (exit reason 48) with the write bit in the qualification. */
    uint64_t page_flags = HYPE_EPT_READ | HYPE_EPT_EXEC | HYPE_EPT_MEMTYPE_WB | HYPE_EPT_PS;
    uint64_t offset;

    for (offset = 0; offset < size; offset += HYPE_PAGING_2MB) {
        uint64_t guest_phys = guest_phys_base + offset;
        uint64_t host_phys = host_phys_base + offset;
        unsigned int gb = (unsigned int)(guest_phys / HYPE_PAGING_1GB);
        unsigned int pd_index = (unsigned int)((guest_phys % HYPE_PAGING_1GB) / HYPE_PAGING_2MB);

        pd_tables[gb][pd_index] = hype_ept_encode_entry(host_phys, page_flags);
    }
}

/*
 * Make a guest-physical 2MB page fault on any access. Zero is the not-present
 * encoding for EPT as it is for ordinary paging: with R/W/X all clear the entry
 * grants nothing, so any access raises an EPT violation (exit reason 48).
 */
void hype_ept_mark_not_present(hype_ept_pte_t pd_tables[][HYPE_EPT_ENTRIES_PER_TABLE],
                               uint64_t phys_addr) {
    unsigned int gb = (unsigned int)(phys_addr / HYPE_PAGING_1GB);
    unsigned int pd_index = (unsigned int)((phys_addr % HYPE_PAGING_1GB) / HYPE_PAGING_2MB);

    pd_tables[gb][pd_index] = 0;
}

void hype_ept_mark_range_not_present(hype_ept_pte_t pd_tables[][HYPE_EPT_ENTRIES_PER_TABLE],
                                     uint64_t base, uint64_t size) {
    uint64_t offset;

    for (offset = 0; offset < size; offset += HYPE_PAGING_2MB) {
        hype_ept_mark_not_present(pd_tables, base + offset);
    }
}

/*
 * #599 (APICv): the one 4 KiB mapping in an otherwise 2 MiB-leaf tree.
 *
 * "Virtualize APIC accesses" fires on a guest-physical access whose EPT
 * translation lands on the APIC-access page, so GPA 0xFEE00000 must be PRESENT
 * and must translate to exactly that host page -- a not-present hole (the
 * pre-APICv arrangement) gives plain EPT violations instead, and a 2 MiB leaf
 * cannot single out one 4 KiB frame. This splits the 2 MiB region containing
 * `gpa_page` through a caller-provided, zeroed page table: one present 4 KiB
 * entry for the APIC page (UC -- it is MMIO-shaped), all 511 siblings left
 * not-present so they keep faulting to the device models exactly as the hole
 * did. Callers pass a table whose storage lives as long as the VM.
 */
void hype_ept_split_map_4k(hype_ept_pte_t pd_tables[][HYPE_EPT_ENTRIES_PER_TABLE],
                           hype_ept_pte_t *pt, uint64_t gpa_page, uint64_t hpa_page) {
    uint64_t table_flags = HYPE_EPT_READ | HYPE_EPT_WRITE | HYPE_EPT_EXEC;
    unsigned int gb = (unsigned int)(gpa_page / HYPE_PAGING_1GB);
    unsigned int pd_index = (unsigned int)((gpa_page % HYPE_PAGING_1GB) / HYPE_PAGING_2MB);
    unsigned int pt_index = (unsigned int)((gpa_page % HYPE_PAGING_2MB) / 4096u);
    unsigned int i;

    for (i = 0; i < HYPE_EPT_ENTRIES_PER_TABLE; i++) {
        pt[i] = 0;
    }
    pt[pt_index] = hype_ept_encode_entry(hpa_page, table_flags | HYPE_EPT_MEMTYPE_UC);
    pd_tables[gb][pd_index] = hype_ept_encode_entry((uint64_t)(uintptr_t)pt, table_flags);
}
