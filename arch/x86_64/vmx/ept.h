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

/* Identity-map coverage. Must span every guest-physical address a VMX guest
 * can touch -- crucially including where hype's OWN static buffers land (the
 * test-guest code blobs live in .efi BSS, which UEFI loads high: observed at
 * ~5GB under QEMU -m 8192). 4GB was too small (EPT-violation / control-field
 * failures for a high-loaded guest); 16GB covers the QEMU RAM sizes hype runs
 * with plus headroom. Revisit once per-VM mem_mb sizing drives this (same
 * rationale as HYPE_NPT_MAX_GB). 16 * 512 * 8B = 64KB of PD tables. */
#define HYPE_EPT_MAX_GB 16

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

/*
 * VMX-4 (#236): the EPT counterparts of hype_npt_map_range() /
 * hype_npt_mark_not_present() / hype_npt_mark_range_not_present(). A live guest
 * needs both -- its RAM is not identity-mapped, and its MMIO windows must fault
 * into hype's device models. Same 2MB-page index arithmetic and the same
 * 2MB-alignment requirements as the NPT versions; only the entry encoding
 * differs. Pure struct-filling, no CPU state touched.
 */
void hype_ept_map_range(hype_ept_pte_t pd_tables[][HYPE_EPT_ENTRIES_PER_TABLE],
                        uint64_t guest_phys_base, uint64_t host_phys_base, uint64_t size);
/* #457: hype_ept_map_range without the WRITE grant -- reads/fetches direct,
 * writes take an EPT violation. Caller owns the INVEPT that makes a runtime
 * edit visible (hype_vmx_vcpu_invept). */
void hype_ept_map_range_ro(hype_ept_pte_t pd_tables[][HYPE_EPT_ENTRIES_PER_TABLE],
                           uint64_t guest_phys_base, uint64_t host_phys_base, uint64_t size);
void hype_ept_mark_not_present(hype_ept_pte_t pd_tables[][HYPE_EPT_ENTRIES_PER_TABLE],
                               uint64_t phys_addr);
void hype_ept_mark_range_not_present(hype_ept_pte_t pd_tables[][HYPE_EPT_ENTRIES_PER_TABLE],
                                     uint64_t base, uint64_t size);

#endif /* HYPE_ARCH_VMX_EPT_H */
