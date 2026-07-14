#ifndef HYPE_ARCH_SVM_NPT_H
#define HYPE_ARCH_SVM_NPT_H

#include "../cpu/paging.h"

/*
 * NPT (Nested Page Tables, M3-1): AMD's NPT reuses the exact same
 * page-table-entry format as ordinary long-mode paging (AMD SDM) --
 * hype_pte_t/hype_paging_encode_entry (arch/x86_64/cpu/paging.h)
 * apply unchanged, same as the host's own M1-3 identity map. The one
 * difference: every level here also sets the User/Supervisor bit.
 * NPT translates guest-*physical* addresses regardless of what
 * privilege level the guest itself is executing at -- the guest's own
 * page tables (if it has any) are what enforce supervisor/user
 * semantics for the guest; NPT sits underneath that translation and
 * must not add its own extra restriction, or any guest-mode (CPL>0)
 * access would fault against NPT before even reaching the guest's own
 * paging.
 */

/* Generous default for a single test/dev guest -- revisit once real
 * per-VM memory sizing (hype.cfg's mem_mb, already summed by ADM-1)
 * drives this instead of a fixed constant. */
#define HYPE_NPT_MAX_GB 4

/*
 * Fills pml4[0] -> pdpt -> pd_tables[0..gb_to_map-1] as a flat
 * identity map (guest-physical == host-physical) via 2MB pages --
 * same shape and ownership/alignment requirements as
 * hype_paging_build_identity(), but with the User/Supervisor bit also
 * set on every level (see above). Pure struct-filling, no CPU state
 * touched.
 */
void hype_npt_build_identity(hype_pte_t *pml4, hype_pte_t *pdpt,
                              hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE],
                              unsigned int gb_to_map);

/*
 * Marks the 2MB PD entry covering `phys_addr` as not-present (M4-3):
 * any guest access anywhere in that 2MB range then takes an NPF
 * (#VMEXIT_NPF) instead of a direct memory access, which is how this
 * project traps MMIO-style devices (the emulated CFI flash,
 * devices/pflash.h, being the first) without needing 4KB-granularity
 * NPT at all -- coarse (whole 2MB), but simple, and correct as long as
 * the device's actual backing region is placed within its own
 * dedicated 2MB range not shared with real guest RAM. `phys_addr` must
 * fall within the range hype_npt_build_identity() already mapped
 * (gb_to_map GB) -- this only clears an existing entry, it doesn't
 * grow the table. Pure struct mutation, no CPU state touched.
 */
void hype_npt_mark_not_present(hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE], uint64_t phys_addr);

#endif /* HYPE_ARCH_SVM_NPT_H */
