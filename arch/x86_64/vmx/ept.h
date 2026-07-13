#ifndef HYPE_ARCH_VMX_EPT_H
#define HYPE_ARCH_VMX_EPT_H

#include <stdint.h>

/*
 * EPT (Extended Page Tables, M3-1). UNVALIDATED -- see vmx.h. Unlike
 * SVM's NPT (which reuses ordinary long-mode page-table-entry
 * encoding verbatim), EPT paging structures use their own distinct
 * entry format (Intel SDM Vol 3C, EPT section): bits 2:0 are
 * separate Read/Write/Execute permission bits (not a single "write"
 * bit gating both, and no user/supervisor bit at all -- EPT applies
 * uniformly regardless of guest CPL), bits 5:3 are an EPT memory type
 * (meaningful only on a leaf entry), bit 6 is "ignore PAT" (leaf
 * only), and bit 7 is the same "this maps a huge page, not another
 * table" flag as ordinary paging, at the same bit position.
 */

typedef uint64_t hype_ept_pte_t;

#define HYPE_EPT_READ (1ULL << 0)
#define HYPE_EPT_WRITE (1ULL << 1)
#define HYPE_EPT_EXEC (1ULL << 2)
#define HYPE_EPT_PS (1ULL << 7)
/* EPT memory type field (bits 5:3), leaf entries only -- 6 = Write-Back,
 * the correct type for ordinary identity-mapped guest RAM. */
#define HYPE_EPT_MEMTYPE_WB (6ULL << 3)

#define HYPE_EPT_ENTRIES_PER_TABLE 512

/* Generous default for a single test/dev guest -- revisit once real
 * per-VM memory sizing (hype.cfg's mem_mb) drives this instead of a
 * fixed constant (same rationale as HYPE_NPT_MAX_GB). */
#define HYPE_EPT_MAX_GB 4

/*
 * Encodes one EPT paging-structure entry (PML4E/PDPTE/PDE, all the
 * same 8-byte shape): `addr` is the next table's (or, with PS set, the
 * final page's) physical address, masked to bits 12-51; `flags` is
 * everything else (R/W/X/memtype/ignore-PAT/PS in bits 0-7) masked to
 * just those bits. Pure bit-packing, no CPU state touched.
 */
uint64_t hype_ept_encode_entry(uint64_t addr, uint64_t flags);

/*
 * Fills pml4[0] -> pdpt -> pd_tables[0..gb_to_map-1] as a flat
 * identity map (guest-physical == host-physical) via 2MB pages, with
 * full R/W/X permissions and Write-Back memory type on every leaf --
 * same shape and ownership/alignment requirements as
 * hype_paging_build_identity()/hype_npt_build_identity(). Pure
 * struct-filling, no CPU state touched.
 */
void hype_ept_build_identity(hype_ept_pte_t *pml4, hype_ept_pte_t *pdpt,
                              hype_ept_pte_t pd_tables[][HYPE_EPT_ENTRIES_PER_TABLE],
                              unsigned int gb_to_map);

#endif /* HYPE_ARCH_VMX_EPT_H */
